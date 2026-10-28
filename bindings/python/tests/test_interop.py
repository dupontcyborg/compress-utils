"""
Canonical-implementation interop tests for compress_utils (Python binding).

The smoke suite in test_compress_utils.py proves *self-consistency*: our
output decodes with our own decompressor, and across our one-shot ↔
streaming APIs. It does NOT prove our output is a valid `.zst` / `.gz` /
`.lz4` / etc. stream in anyone else's tools, nor that we accept input we
didn't produce. That gap is exactly what let the legacy LZ4 wire-format
bug live: it round-tripped against itself but was never fed to a
canonical LZ4 frame parser.

This module closes the gap for every algorithm, in both directions,
against an *independent* reference implementation:

    | algorithm | canonical reference        | independent of our bundle? |
    |-----------|----------------------------|----------------------------|
    | zstd      | `zstandard` (PyPI)         | yes (own libzstd)          |
    | brotli    | `brotli` (PyPI)            | yes (own libbrotli)        |
    | zlib      | `zlib` (stdlib)            | yes (system zlib)          |
    | bz2       | `bz2` (stdlib)             | yes (system libbz2)        |
    | lz4       | `lz4.frame` (PyPI)         | yes (own liblz4)           |
    | xz        | `lzma` (stdlib)            | yes (system liblzma)       |
    | snappy    | `snappy` (python-snappy)   | yes (own libsnappy)        |
    | gzip      | `gzip` (stdlib)            | yes (system zlib)          |
    | deflate   | `zlib` (stdlib, wbits=-15) | yes (system zlib)          |
    | lz4_raw   | `lz4.block` (PyPI)         | yes (own liblz4)           |

For each (algorithm, payload) we assert:
    outbound: cu.compress(x)        -> canonical.decompress(...) == x
    inbound:  canonical.compress(x) -> cu.decompress(...)        == x

Both directions matter. Outbound proves we emit valid streams; inbound
proves we accept streams we didn't make (where bugs like "ZSTD frame
without pledged size is rejected" surface).

The PyPI references (zstandard, brotli, lz4) are optional: if one isn't
installed, that algorithm's tests are skipped rather than failed, so the
suite stays green on a bare `pip install pytest` checkout. CI installs
them (see pyproject `[project.optional-dependencies].test` and the
cibuildwheel `test-requires`) so the full matrix actually runs.

The wire format each algorithm produces is documented in the C core
(include/compress_utils.h and src/algorithms/<algo>/<algo>.c). The
canonical decoder chosen here must match that format exactly:
  - zstd   : zstd frame WITH content-size  -> zstandard default decoder
  - brotli : raw brotli stream             -> brotli.decompress
  - zlib   : zlib wrapper (RFC 1950)       -> zlib.decompress (NOT gzip, NOT raw)
  - bz2    : bzip2 stream                  -> bz2.decompress
  - lz4    : LZ4 frame (.lz4) w/ checksum  -> lz4.frame.decompress
  - xz     : .xz stream w/ CRC64           -> lzma.decompress (FORMAT_XZ default)
  - snappy : raw Snappy block              -> snappy.decompress (NOT framed .sz)
  - gzip   : gzip stream (RFC 1952)        -> gzip.decompress (NOT raw zlib/RFC 1950)
  - deflate: raw DEFLATE (RFC 1951)        -> zlib with wbits=-15
  - lz4_raw: raw LZ4 block, no size prefix -> lz4.block with store_size=False;
             both sides need the decompressed size, so it has its own test
"""

import itertools
import random
import unittest
import zlib

import compress_utils as cu


# --- payloads --------------------------------------------------------------
#
# Small/edge plus larger and binary inputs. Empty input is included
# deliberately: several frame formats encode an empty payload specially,
# and the legacy code had empty-stream bugs.

_TEXT = b"The quick brown fox jumps over the lazy dog. "

PAYLOADS = {
    "empty":          b"",
    "single_byte":    b"A",
    "text":           _TEXT,
    "text_16k":       (_TEXT * ((16 * 1024) // len(_TEXT) + 1))[: 16 * 1024],
    "binary_4k":      bytes(range(256)) * 16,
    # Highly compressible — stresses long back-references / RLE paths.
    "repetitive_64k": b"y" * (64 * 1024),
}


# --- canonical references --------------------------------------------------
#
# Each entry: (compress_fn, decompress_fn). Built lazily so a missing
# optional dependency only skips its own algorithm. `None` means the
# reference library isn't importable in this environment.

def _zstd_ref():
    try:
        import zstandard
    except ImportError:
        return None
    # ZstdDecompressor needs a max_output_size for frames without a
    # content-size; ours always sets content-size, but stream_reader is
    # robust either way and avoids that constraint.
    def _c(data):
        return zstandard.ZstdCompressor().compress(data)
    def _d(data):
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)
    return (_c, _d)


def _brotli_ref():
    try:
        import brotli
    except ImportError:
        return None
    return (brotli.compress, brotli.decompress)


def _zlib_ref():
    import zlib  # stdlib — always present
    return (zlib.compress, zlib.decompress)


def _bz2_ref():
    import bz2  # stdlib
    return (bz2.compress, bz2.decompress)


def _lz4_ref():
    try:
        import lz4.frame as lz4f
    except ImportError:
        return None
    return (lz4f.compress, lz4f.decompress)


def _xz_ref():
    import lzma  # stdlib
    return (lzma.compress, lzma.decompress)


def _snappy_ref():
    try:
        import snappy  # python-snappy
    except ImportError:
        return None
    # Plain snappy.compress/decompress use the raw block format (a varint
    # length prefix + the compressed stream) — the same format we emit.
    # NOT snappy.framed / hadoop_snappy, which are stream-framing formats.
    return (snappy.compress, snappy.decompress)


def _gzip_ref():
    import gzip  # stdlib
    # gzip.compress/decompress use the gzip container (RFC 1952) — the same
    # format we emit — as opposed to zlib.compress (RFC 1950 wrapper).
    return (gzip.compress, gzip.decompress)


def _deflate_ref():
    import zlib  # stdlib
    def _c(data):
        c = zlib.compressobj(wbits=-15)
        return c.compress(data) + c.flush()
    def _d(data):
        return zlib.decompress(data, wbits=-15)
    return (_c, _d)


REFERENCES = {
    "zstd":   _zstd_ref,
    "brotli": _brotli_ref,
    "zlib":   _zlib_ref,
    "bz2":    _bz2_ref,
    "lz4":    _lz4_ref,
    "xz":     _xz_ref,
    "snappy": _snappy_ref,
    "gzip":   _gzip_ref,
    "deflate": _deflate_ref,
}


# --- mid-stream flushing senders --------------------------------------------
#
# Each returns send(payload) -> the bytes a canonical encoder emits for that
# payload followed by a nonterminal flush, all in one open stream, or None if
# the reference isn't installed. bz2 and lzma have no nonterminal flush.

def _zstd_flushing():
    try:
        import zstandard as zstd
    except ImportError:
        return None
    enc = zstd.ZstdCompressor(level=11).compressobj()
    return lambda data: enc.compress(data) + enc.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)


def _zstd_flushing_minimal_frame():
    # The issue's real-world sender: level 3, 4 MiB window, no content size,
    # no checksum.
    try:
        import zstandard as zstd
    except ImportError:
        return None
    params = zstd.ZstdCompressionParameters.from_level(
        3, window_log=22, write_content_size=False, write_checksum=False)
    enc = zstd.ZstdCompressor(compression_params=params).compressobj()
    return lambda data: enc.compress(data) + enc.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)


def _lz4_flushing():
    try:
        import lz4.frame as lz4f
    except ImportError:
        return None
    # auto_flush emits each compress() call's blocks: LZ4F's mid-stream flush.
    # (python-lz4's flush() ends the frame.)
    enc = lz4f.LZ4FrameCompressor(block_linked=True, auto_flush=True)
    header = [enc.begin()]
    return lambda data: (header.pop() if header else b"") + enc.compress(data)


def _brotli_flushing():
    try:
        import brotli
    except ImportError:
        return None
    enc = brotli.Compressor()
    return lambda data: enc.process(data) + enc.flush()


def _zlib_flushing(wbits):
    def make():
        enc = zlib.compressobj(6, zlib.DEFLATED, wbits)
        return lambda data: enc.compress(data) + enc.flush(zlib.Z_SYNC_FLUSH)
    return make


# Each returns receive(chunk) -> the bytes a canonical streaming decoder emits
# for that chunk, or None if the reference isn't installed.

def _zstd_receiver():
    try:
        import zstandard as zstd
    except ImportError:
        return None
    return zstd.ZstdDecompressor().decompressobj().decompress


def _lz4_receiver():
    try:
        import lz4.frame as lz4f
    except ImportError:
        return None
    # Plain decompress() keeps output LZ4F still holds once input runs out,
    # even for python-lz4's own auto-flushed streams; max_length mode drains it.
    dec = lz4f.LZ4FrameDecompressor()

    def receive(data):
        out = dec.decompress(data, max_length=1 << 20)
        while not dec.needs_input:
            more = dec.decompress(b"", max_length=1 << 20)
            if not more:
                break
            out += more
        return out
    return receive


def _brotli_receiver():
    try:
        import brotli
    except ImportError:
        return None
    # process() hands out decoded output in portions; empty calls get the rest.
    dec = brotli.Decompressor()

    def receive(data):
        out = dec.process(data)
        while True:
            more = dec.process(b"")
            if not more:
                return out
            out += more
    return receive


def _xz_receiver():
    import lzma
    return lzma.LZMADecompressor().decompress


def _zlib_receiver(wbits):
    return lambda: zlib.decompressobj(wbits).decompress


FLUSHING_RECEIVERS = {
    "zstd": _zstd_receiver,
    "lz4": _lz4_receiver,
    "brotli": _brotli_receiver,
    "xz": _xz_receiver,
    "zlib": _zlib_receiver(15),
    "gzip": _zlib_receiver(31),
    "deflate": _zlib_receiver(-15),
}


# (label, algorithm, sender factory)
FLUSHING_SENDERS = [
    ("zstd", "zstd", _zstd_flushing),
    ("zstd, level 3, no size or checksum", "zstd", _zstd_flushing_minimal_frame),
    ("lz4", "lz4", _lz4_flushing),
    ("brotli", "brotli", _brotli_flushing),
    ("zlib", "zlib", _zlib_flushing(15)),
    ("gzip", "gzip", _zlib_flushing(31)),
    ("deflate", "deflate", _zlib_flushing(-15)),
]


def _flush_sequences():
    """The issue's message patterns, each a list of messages flushed in turn."""
    rng = random.Random(54)
    record = rng.randbytes(422)
    return {
        "issue repro": [b"A" * 131072, b"B" * 37],
        "64K/128K mix": [b"A" * 65535, rng.randbytes(65536), b"A" * 65537,
                         rng.randbytes(131071), b"A" * 131072, rng.randbytes(131073), b"x"],
        "record, 65537, 37": [record, b"A" * 65537, b"B" * 37],
        "record + 100 small": [record] + [b"msg %05d " % i + rng.randbytes(88) for i in range(100)],
    }


# The issue's transport profiles: compressed bytes per receiver call, cycled
# across the whole stream. None = each flushed batch in one call.
TRANSPORTS = {
    "whole": lambda: None,
    "4096": lambda: itertools.repeat(4096),
    "1,3,7,97,4093": lambda: itertools.cycle([1, 3, 7, 97, 4093]),
    "1": lambda: itertools.repeat(1),
}


def _deliver(wire, transport, receive):
    """Hands `wire` to `receive` sliced by the transport; returns the output."""
    if transport is None:
        return receive(wire)
    out, pos = b"", 0
    while pos < len(wire):
        step = next(transport)
        out += receive(wire[pos:pos + step])
        pos += step
    return out


class TestCanonicalInterop(unittest.TestCase):
    """ours <-> canonical reference, both directions, every algorithm."""

    def _reference_for(self, algo):
        if not cu.is_available(algo):
            self.skipTest(f"{algo} not compiled into this build")
        ref = REFERENCES[algo]()
        if ref is None:
            self.skipTest(f"canonical reference library for {algo} not installed")
        return ref

    def test_outbound(self):
        """cu.compress(x) must decode with the canonical reference."""
        for algo in REFERENCES:
            for case, data in PAYLOADS.items():
                with self.subTest(direction="outbound", algorithm=algo, case=case):
                    _, ref_decompress = self._reference_for(algo)
                    blob = cu.compress(data, algo, level=5)
                    restored = ref_decompress(blob)
                    self.assertEqual(
                        restored, data,
                        f"{algo}: canonical decoder did not recover our output "
                        f"({case}, {len(data)} bytes -> {len(blob)} compressed)",
                    )

    def test_inbound(self):
        """Output of the canonical reference must decode with cu.decompress."""
        for algo in REFERENCES:
            for case, data in PAYLOADS.items():
                with self.subTest(direction="inbound", algorithm=algo, case=case):
                    ref_compress, _ = self._reference_for(algo)
                    blob = ref_compress(data)
                    restored = cu.decompress(blob, algo)
                    self.assertEqual(
                        restored, data,
                        f"{algo}: we did not recover canonical output "
                        f"({case}, {len(data)} bytes -> {len(blob)} compressed)",
                    )


    def test_inbound_flush_checkpoints(self):
        """A canonical encoder's mid-stream flushes decode completely at every
        checkpoint, with no further input and no finish(), under every
        transport profile (issue #54). Empty calls afterwards return nothing."""
        for label, algo, make in FLUSHING_SENDERS:
            for seq_name, messages in _flush_sequences().items():
                for transport_name, transport in TRANSPORTS.items():
                    with self.subTest(sender=label, sequence=seq_name, transport=transport_name):
                        if not cu.is_available(algo):
                            self.skipTest(f"{algo} not compiled into this build")
                        send = make()
                        if send is None:
                            self.skipTest(f"canonical reference library for {algo} not installed")
                        receiver = cu.DecompressStream(algo)
                        slices = transport()
                        got = want = b""
                        for i, data in enumerate(messages):
                            got += _deliver(send(data), slices, receiver.decompress)
                            want += data
                            self.assertEqual(len(got), len(want), f"checkpoint {i}")
                            self.assertEqual(got, want, f"checkpoint {i}")
                            for _ in range(3):
                                self.assertEqual(receiver.decompress(b""), b"")

    def test_outbound_flush_checkpoints(self):
        """After our flush(), a canonical streaming decoder recovers everything
        written so far, with the stream still open, under every transport
        profile (issue #54)."""
        for algo, make in FLUSHING_RECEIVERS.items():
            for seq_name, messages in _flush_sequences().items():
                for transport_name, transport in TRANSPORTS.items():
                    with self.subTest(algorithm=algo, sequence=seq_name, transport=transport_name):
                        if not cu.is_available(algo):
                            self.skipTest(f"{algo} not compiled into this build")
                        receive = make()
                        if receive is None:
                            self.skipTest(f"canonical reference library for {algo} not installed")
                        sender = cu.CompressStream(algo)
                        slices = transport()
                        got = want = b""
                        for i, data in enumerate(messages):
                            got += _deliver(sender.compress(data) + sender.flush(), slices, receive)
                            want += data
                            self.assertEqual(len(got), len(want), f"checkpoint {i}")
                            self.assertEqual(got, want, f"checkpoint {i}")
                        got += _deliver(sender.finish(), slices, receive)
                        self.assertEqual(got, want)

    def test_lz4_raw(self):
        """Raw LZ4 blocks interoperate with lz4.block in both directions."""
        if not cu.is_available("lz4_raw"):
            self.skipTest("lz4_raw not compiled into this build")
        try:
            import lz4.block as lz4b
        except ImportError:
            self.skipTest("canonical reference library for lz4_raw not installed")
        for case, data in PAYLOADS.items():
            with self.subTest(algorithm="lz4_raw", case=case):
                ours = cu.compress(data, "lz4_raw")
                self.assertEqual(lz4b.decompress(ours, uncompressed_size=len(data)), data)
                theirs = lz4b.compress(data, store_size=False)
                self.assertEqual(cu.decompress(theirs, "lz4_raw", expected_size=len(data)), data)


class TestXzFittedDictionary(unittest.TestCase):
    """One-shot xz caps the dictionary at the input size (4 KiB floor, never above
    the preset's): output stays a standard .xz that Python's own lzma decodes, and
    a decoder needs far less memory than the preset's 8 MiB dictionary."""

    SIZES = [0, 1, 4095, 4096, 4097, 8191, 8192, 8193, 65535, 65536, 65537]

    def _data(self, n):
        rng = random.Random(n)
        words = [rng.randbytes(rng.randint(2, 9)) for _ in range(300)]
        out = bytearray()
        while len(out) < n:
            out += rng.choice(words) + b" "
        return bytes(out[:n])

    def test_round_trip_around_the_cap(self):
        import lzma
        for level in (1, 6, 10):
            for n in self.SIZES:
                with self.subTest(level=level, size=n):
                    data = self._data(n)
                    packed = cu.compress(data, "xz", level=level)
                    self.assertEqual(cu.decompress(packed, "xz"), data)
                    self.assertEqual(lzma.decompress(packed), data)

    def test_decoder_memory_follows_the_input(self):
        import lzma
        for n in (1, 4096, 4097, 65536):
            with self.subTest(size=n):
                data = self._data(n)
                packed = cu.compress(data, "xz", level=6)
                # Level 6's preset dictionary is 8 MiB; a fitted one fits well under 1 MiB.
                dec = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=1 << 20)
                self.assertEqual(dec.decompress(packed), data)
        # Above the preset's dictionary the cap does not apply: level 1 is 256 KiB.
        data = self._data(300_000)
        packed = cu.compress(data, "xz", level=1)
        self.assertEqual(lzma.decompress(packed), data)
        with self.assertRaises(lzma.LZMAError):
            lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=128 << 10).decompress(packed)


if __name__ == "__main__":
    unittest.main()
