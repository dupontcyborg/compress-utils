#!/usr/bin/env python3
"""
Python benchmark driver, for compress-utils and for the Python libraries it
competes with.

Speaks the shared benchmark driver protocol (see benchmarks/README.md): reads
"<algo> <level> [<mode>] <path>" job lines from stdin (mode: oneshot, stream or
msgset), emits one NDJSON result (or skip/error marker) per line, honours
BENCH_SAMPLES / BENCH_WARMUP / BENCH_CHUNK, and answers `--info`.

`--impl` picks the implementation (default compress-utils); every one goes
through the same timing loop, as a consumer would call it:

  oneshot  compress, then decompress without being told the size
  msgset   one compress call per message, each decompressed with its known
           size through the library's fastest path for that (compress-utils'
           expected_size, zlib's bufsize, cramjam's output_len, ...)
  stream   the input fed in BENCH_CHUNK pieces through the library's streaming
           API; skipped when it has none (cramjam can't decompress chunked
           input; the runner records nothing for that pair)

Competitors run at native levels (the runner translates through levels.json,
or sweeps the range each codec declares here) and are set to compress-utils'
wire format where their API allows: zstd frames carry the content size, LZ4
frames the content size and a content checksum, xz streams a CRC64. Each
competitor's output is also decoded by compress-utils once per job, untimed,
and the record carries the result as `interop`.

    python3 bench_py.py [--impl NAME] [--info]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "bindings" / "python"))
sys.path.insert(0, str(REPO / "benchmarks" / "corpus"))
import compress_utils as cu  # noqa: E402
import msgset  # noqa: E402

SAMPLES = int(os.environ.get("BENCH_SAMPLES") or 5)
WARMUP = int(os.environ.get("BENCH_WARMUP") or 1)
CHUNK = int(os.environ.get("BENCH_CHUNK") or 64 * 1024)


@dataclass
class Codec:
    """One implementation of one format. decompress(data, size) gets the
    decompressed size when the caller knows it (msgset), else None."""
    compress: Callable[[bytes, int], object]
    decompress: Callable[[object, int | None], object]
    cstream: Callable[[bytes, int], object] | None = None
    dstream: Callable[[object], object] | None = None
    levels: tuple[int, int] | None = None  # native range; None = levels.json's


def _chunks(data):
    mv = memoryview(data)
    return (mv[off:off + CHUNK] for off in range(0, len(mv), CHUNK))


def _feed(obj, method: str, data, tail: str | None) -> bytes:
    """Run `data` through obj.<method> in chunks, then obj.<tail>()."""
    step = getattr(obj, method)
    parts = [step(c) for c in _chunks(data)]
    if tail:
        parts.append(getattr(obj, tail)())
    return b"".join(parts)


# --------------------------------------------------------------------------- #
# Implementations
# --------------------------------------------------------------------------- #


def impl_compress_utils() -> tuple[str, dict[str, Codec]]:
    def codec(algo: str) -> Codec:
        a = getattr(cu.Algorithm, algo)
        return Codec(
            compress=lambda d, lvl: cu.compress(d, a, lvl),
            decompress=lambda c, n: cu.decompress(c, a, expected_size=n) if n is not None
            else cu.decompress(c, a),
            cstream=lambda d, lvl: _feed(cu.CompressStream(a, lvl), "compress", d, "finish"),
            dstream=lambda c: _feed(cu.DecompressStream(a), "decompress", c, "finish"),
        )
    return cu.version(), {a: codec(a) for a in
                          ("zstd", "brotli", "zlib", "gzip", "deflate", "bz2", "lz4", "xz",
                           "snappy")}


def impl_stdlib() -> tuple[str, dict[str, Codec]]:
    import bz2
    import lzma
    import platform
    import zlib
    from compression import zstd

    def zlib_codec(wbits: int) -> Codec:
        return Codec(
            compress=lambda d, lvl: zlib.compress(d, lvl, wbits=wbits),
            decompress=lambda c, n: zlib.decompress(c, wbits=wbits, bufsize=n) if n
            else zlib.decompress(c, wbits=wbits),
            cstream=lambda d, lvl: _feed(zlib.compressobj(lvl, zlib.DEFLATED, wbits),
                                         "compress", d, "flush"),
            dstream=lambda c: _feed(zlib.decompressobj(wbits), "decompress", c, "flush"),
        )

    codecs = {
        "zlib": zlib_codec(15),
        "gzip": zlib_codec(31),
        "deflate": zlib_codec(-15),
        "bz2": Codec(
            compress=lambda d, lvl: bz2.compress(d, lvl),
            decompress=lambda c, n: bz2.decompress(c),
            cstream=lambda d, lvl: _feed(bz2.BZ2Compressor(lvl), "compress", d, "flush"),
            dstream=lambda c: _feed(bz2.BZ2Decompressor(), "decompress", c, None),
        ),
        "xz": Codec(
            compress=lambda d, lvl: lzma.compress(d, lzma.FORMAT_XZ, lzma.CHECK_CRC64, lvl),
            decompress=lambda c, n: lzma.decompress(c, lzma.FORMAT_XZ),
            cstream=lambda d, lvl: _feed(
                lzma.LZMACompressor(lzma.FORMAT_XZ, lzma.CHECK_CRC64, lvl), "compress", d,
                "flush"),
            dstream=lambda c: _feed(lzma.LZMADecompressor(lzma.FORMAT_XZ), "decompress", c,
                                    None),
        ),
        "zstd": Codec(
            compress=lambda d, lvl: zstd.compress(d, lvl),
            decompress=lambda c, n: zstd.decompress(c),
            cstream=lambda d, lvl: _feed(zstd.ZstdCompressor(lvl), "compress", d, "flush"),
            dstream=lambda c: _feed(zstd.ZstdDecompressor(), "decompress", c, None),
        ),
    }
    version = (f"python {platform.python_version()}, zlib {zlib.ZLIB_RUNTIME_VERSION}, "
               f"zstd {'.'.join(map(str, zstd.zstd_version_info))}")
    return version, codecs


def impl_cramjam() -> tuple[str, dict[str, Codec]]:
    import cramjam

    # cramjam returns its own Buffer; it's left as is in the timed path (it
    # supports the buffer protocol), as a caller would use it. Its streaming
    # Compressor works, but its Decompressor needs a whole stream per call, so
    # there is no chunked decoding to benchmark: no dstream.
    def codec(mod, levels, **kw) -> Codec:
        return Codec(
            compress=lambda d, lvl: mod.compress(d, level=lvl, **kw),
            decompress=lambda c, n: mod.decompress(c, output_len=n),
            levels=levels,
        )

    xz = cramjam.xz
    codecs = {
        "zstd": codec(cramjam.zstd, (1, 22)),
        "zlib": codec(cramjam.zlib, (1, 9)),
        "gzip": codec(cramjam.gzip, (1, 9)),
        "deflate": codec(cramjam.deflate, (1, 9)),
        "bz2": codec(cramjam.bzip2, (1, 9)),
        "lz4": codec(cramjam.lz4, (0, 12)),
        "brotli": codec(cramjam.brotli, (0, 11)),
        "xz": Codec(
            compress=lambda d, lvl: xz.compress(d, preset=lvl, format=xz.Format.XZ,
                                                check=xz.Check.Crc64),
            decompress=lambda c, n: xz.decompress(c, output_len=n),
            levels=(0, 9),
        ),
        "snappy": Codec(
            compress=lambda d, lvl: cramjam.snappy.compress_raw(d),
            decompress=lambda c, n: cramjam.snappy.decompress_raw(c, output_len=n),
        ),
    }
    from importlib.metadata import version
    return version("cramjam"), codecs


def impl_zstandard() -> tuple[str, dict[str, Codec]]:
    import zstandard

    # A compressor per level and one decompressor, reused: the library's
    # documented usage. Frames carry the content size by default.
    cctx: dict[int, zstandard.ZstdCompressor] = {}
    dctx = zstandard.ZstdDecompressor()

    def c(lvl):
        if lvl not in cctx:
            cctx[lvl] = zstandard.ZstdCompressor(level=lvl)
        return cctx[lvl]

    return zstandard.__version__, {"zstd": Codec(
        compress=lambda d, lvl: c(lvl).compress(d),
        decompress=lambda b, n: dctx.decompress(b, max_output_size=n) if n else dctx.decompress(b),
        cstream=lambda d, lvl: _feed(c(lvl).compressobj(), "compress", d, "flush"),
        dstream=lambda b: _feed(dctx.decompressobj(), "decompress", b, None),
        levels=(1, 22),
    )}


def impl_brotli() -> tuple[str, dict[str, Codec]]:
    import brotli

    return brotli.version, {"brotli": Codec(
        compress=lambda d, lvl: brotli.compress(d, quality=lvl),
        decompress=lambda c, n: brotli.decompress(c),
        cstream=lambda d, lvl: _feed(brotli.Compressor(quality=lvl), "process", d, "finish"),
        dstream=lambda c: _feed(brotli.Decompressor(), "process", c, None),
        levels=(0, 11),
    )}


def impl_lz4() -> tuple[str, dict[str, Codec]]:
    import lz4
    import lz4.frame as f

    # LZ4 frames like compress-utils': content size and content checksum.
    return lz4.__version__, {"lz4": Codec(
        compress=lambda d, lvl: f.compress(d, compression_level=lvl, content_checksum=True,
                                           store_size=True),
        decompress=lambda c, n: f.decompress(c),
        cstream=lambda d, lvl: _lz4_stream(f, d, lvl),
        dstream=lambda c: _feed(f.LZ4FrameDecompressor(), "decompress", c, None),
        levels=(0, 12),
    )}


def _lz4_stream(f, data, lvl) -> bytes:
    comp = f.LZ4FrameCompressor(compression_level=lvl, content_checksum=True)
    return comp.begin() + _feed(comp, "compress", data, "flush")


IMPLS = {
    "compress-utils": impl_compress_utils,
    "stdlib": impl_stdlib,
    "cramjam": impl_cramjam,
    "zstandard": impl_zstandard,
    "brotli": impl_brotli,
    "lz4": impl_lz4,
}


# --------------------------------------------------------------------------- #
# Timing
# --------------------------------------------------------------------------- #


def _stats(samples: list[int]) -> dict:
    s = sorted(samples)
    n = len(s)
    med = s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2
    dev = sorted(abs(x - med) for x in s)
    mad = dev[n // 2] if n % 2 else (dev[n // 2 - 1] + dev[n // 2]) / 2
    return {"median": round(med), "mad": round(mad), "min": s[0]}


def _timed(fn, runs: int) -> tuple[object, list[int]]:
    """Run fn WARMUP + runs times; time the last `runs`."""
    out, times = None, []
    for r in range(WARMUP + runs):
        t0 = time.perf_counter_ns()
        out = fn()
        if r >= WARMUP:
            times.append(time.perf_counter_ns() - t0)
    return out, times


def _interop(impl: str, algo: str, comp, data: bytes) -> bool | None:
    """Can compress-utils decode a competitor's output? None for ourselves."""
    if impl == "compress-utils":
        return None
    try:
        return cu.decompress(bytes(comp), getattr(cu.Algorithm, algo)) == data
    except Exception:  # noqa: BLE001
        return False


def _record(impl: str, algo: str, level: int, mode: str, path: str, in_bytes: int,
            out_bytes: int, c_t: list[int], d_t: list[int], verified: bool,
            interop: bool | None, messages: int = 0) -> dict:
    c, d = _stats(c_t), _stats(d_t)
    rec = {
        "lang": "python", "impl": impl, "algo": algo, "level": level, "mode": mode,
        "chunk_bytes": CHUNK if mode == "stream" else 0, "input": path,
        "input_bytes": in_bytes, "output_bytes": out_bytes,
        "compress_ns_median": c["median"], "compress_ns_mad": c["mad"], "compress_ns_min": c["min"],
        "decompress_ns_median": d["median"], "decompress_ns_mad": d["mad"],
        "decompress_ns_min": d["min"],
        "samples": SAMPLES, "warmup": WARMUP, "verified": verified,
    }
    if messages:
        rec.update(messages=messages, decompress_size_known=True)
    if interop is not None:
        rec["interop"] = interop
    return rec


def run_job(impl: str, algo: str, codec: Codec, level: int, mode: str, path: str) -> dict:
    data = Path(path).read_bytes()
    if mode == "stream":
        comp, c_t = _timed(lambda: codec.cstream(data, level), SAMPLES)
        dec, d_t = _timed(lambda: codec.dstream(comp), SAMPLES)
    else:
        comp, c_t = _timed(lambda: codec.compress(data, level), SAMPLES)
        dec, d_t = _timed(lambda: codec.decompress(comp, None), SAMPLES)
    return _record(impl, algo, level, mode, path, len(data), len(memoryview(comp)), c_t, d_t,
                   bytes(dec) == data, _interop(impl, algo, comp, data))


def run_msgset_job(impl: str, algo: str, codec: Codec, level: int, path: str) -> dict:
    messages = msgset.read(Path(path))
    comp, c_t = _timed(lambda: [codec.compress(m, level) for m in messages], SAMPLES)

    def decode_all():
        for c, m in zip(comp, messages):
            codec.decompress(c, len(m))

    _, d_t = _timed(decode_all, SAMPLES)
    verified = all(bytes(codec.decompress(c, len(m))) == m for c, m in zip(comp, messages))
    interop = _interop(impl, algo, comp[0], messages[0]) if messages else None
    return _record(impl, algo, level, "msgset", path, sum(map(len, messages)),
                   sum(len(memoryview(c)) for c in comp), c_t, d_t, verified, interop,
                   messages=len(messages))


# --------------------------------------------------------------------------- #
# Protocol
# --------------------------------------------------------------------------- #


def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


JOB_RE = re.compile(r"^(\S+)\s+(\S+)\s+(.*)$")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--impl", default="compress-utils", choices=list(IMPLS))
    ap.add_argument("--info", action="store_true")
    args = ap.parse_args()
    version, codecs = IMPLS[args.impl]()

    if args.info:
        driver = "python" if args.impl == "compress-utils" else f"python-{args.impl}"
        _emit({"lang": "python", "version": version, "driver": driver, "impl": args.impl,
               "levels": {a: list(c.levels) for a, c in codecs.items() if c.levels}})
        return

    # readline loop (not `for line in sys.stdin`) to avoid read-ahead buffering
    # that would deadlock the runner's line-synchronous protocol.
    for raw in iter(sys.stdin.readline, ""):
        line = raw.strip()
        if not line:
            continue
        m = JOB_RE.match(line)
        if not m:
            _emit({"error": True})
            continue
        algo, level_s, rest = m.group(1), m.group(2), m.group(3)
        mode = "oneshot"
        for mo in ("stream", "msgset", "oneshot"):
            if rest.startswith(mo + " "):
                mode, rest = mo, rest[len(mo) + 1:]
                break
        path = rest.strip()

        codec = codecs.get(algo)
        if codec is None or (mode == "stream" and not (codec.cstream and codec.dstream)):
            _emit({"skipped": True})
            continue
        try:
            if mode == "msgset":
                _emit(run_msgset_job(args.impl, algo, codec, int(level_s), path))
            else:
                _emit(run_job(args.impl, algo, codec, int(level_s), mode, path))
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"bench-py[{args.impl}]: {algo} L{level_s} {mode} failed: {e}\n")
            _emit({"error": True})


if __name__ == "__main__":
    main()
