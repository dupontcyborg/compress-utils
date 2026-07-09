#!/usr/bin/env python3
"""
CLI cross-check: round-trip our compressed output against the canonical
command-line tools, and vice versa.

This is a second, independent interop channel alongside the Python-library
interop tests (bindings/python/tests/test_interop.py). Where that suite
checks our output against PyPI/stdlib decoders, this one checks it against
the actual reference *binaries* everyone uses — `zstd`, `xz`, `lz4`,
`bzip2`, `brotli`, `gzip` — exercising the on-disk file formats (`.zst`,
`.xz`, `.lz4`, `.bz2`, `.br`, `.gz`). It's the channel that would have caught
the legacy LZ4 wire-format bug: the `lz4` CLI is *the* canonical LZ4-frame
consumer.

It drives "our side" through the installed `compress_utils` Python binding
(the project ships no standalone CLI yet — see TODO.md). Set PYTHONPATH to
the binding dir if running from a source tree, e.g.:

    PYTHONPATH=bindings/python python3 tests/interop/cli_crosscheck.py

For each algorithm with its CLI available, both directions are checked:
    outbound: cu.compress(x) | <tool> -d        == x
    inbound:  x | <tool> -c   -> cu.decompress() == x

Tools that aren't installed are SKIPPED (not failed), so this is safe to
run anywhere; it only fails on a genuine round-trip mismatch. Exit code:
    0  every available tool round-tripped (or everything was skipped)
    1  at least one real mismatch / error

`zlib` has no entry: our zlib algorithm emits a raw RFC-1950 zlib stream,
which no standard CLI reads (`gzip` is RFC-1952, a different container).
That format is covered by the stdlib `zlib` channel in test_interop.py.

`snappy` has no entry either: our snappy algorithm emits the raw Snappy
block format, and there's no ubiquitous CLI that reads it (`snzip` and
friends use the Snappy *framing* format, a different container). It's
covered against the independent `python-snappy` library in test_interop.py.
"""

import shutil
import subprocess
import sys

try:
    import compress_utils as cu
except ImportError as e:  # pragma: no cover - environment guard
    print(f"SKIP: compress_utils binding not importable ({e}).")
    print("      Set PYTHONPATH to the built binding dir (e.g. bindings/python).")
    # Not a failure of the library under test — nothing to cross-check.
    sys.exit(0)


# algorithm -> (decompress-to-stdout argv, compress-stdin-to-stdout argv)
# All tools read stdin / write stdout with these flags.
TOOLS = {
    "zstd":   (["zstd", "-dc"],   ["zstd", "-q", "-c"]),
    "xz":     (["xz", "-dc"],     ["xz", "-c"]),
    "lz4":    (["lz4", "-dc"],    ["lz4", "-q", "-c"]),
    "bz2":    (["bzip2", "-dc"],  ["bzip2", "-c"]),
    "brotli": (["brotli", "-dc"], ["brotli", "-c"]),
    "gzip":   (["gzip", "-dc"],   ["gzip", "-c"]),
}

# A mix of sizes; binary bytes catch framing/escaping bugs, and the large
# repetitive payload exercises block boundaries in the frame formats.
PAYLOADS = {
    "text":           b"the quick brown fox jumps over the lazy dog\n" * 64,
    "binary_4k":      bytes(range(256)) * 16,
    "repetitive_64k": b"z" * (64 * 1024),
    "single_byte":    b"Q",
}


def _run(argv, data):
    """Pipe `data` through `argv`, returning stdout. Raises on nonzero exit."""
    # `input=` already wires stdin to a pipe, so the tool can never block
    # on a tty/prompt (and passing stdin= alongside input= is an error).
    proc = subprocess.run(
        argv,
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"{argv[0]} exited {proc.returncode}: "
            f"{proc.stderr.decode('utf-8', 'replace').strip()}"
        )
    return proc.stdout


def check_algorithm(algo, decompress_argv, compress_argv):
    """Return (ran, failures) for one algorithm across all payloads."""
    tool = decompress_argv[0]
    if not cu.is_available(algo):
        print(f"  {algo:7} SKIP — not compiled into this build")
        return (False, 0)
    if shutil.which(tool) is None:
        print(f"  {algo:7} SKIP — `{tool}` CLI not installed")
        return (False, 0)

    failures = 0
    for case, data in PAYLOADS.items():
        # outbound: our compressor -> canonical CLI decompressor
        try:
            recovered = _run(decompress_argv, cu.compress(data, algo, level=5))
            if recovered != data:
                print(f"  {algo:7} FAIL outbound[{case}]: "
                      f"`{tool} -d` did not recover our output")
                failures += 1
        except Exception as e:
            print(f"  {algo:7} FAIL outbound[{case}]: {e}")
            failures += 1

        # inbound: canonical CLI compressor -> our decompressor
        try:
            recovered = cu.decompress(_run(compress_argv, data), algo)
            if recovered != data:
                print(f"  {algo:7} FAIL inbound[{case}]: "
                      f"we did not recover `{tool} -c` output")
                failures += 1
        except Exception as e:
            print(f"  {algo:7} FAIL inbound[{case}]: {e}")
            failures += 1

    if failures == 0:
        print(f"  {algo:7} OK   — both directions, {len(PAYLOADS)} payloads vs `{tool}`")
    return (True, failures)


def main():
    print(f"CLI cross-check (compress_utils {cu.version()})")
    ran_any = False
    total_failures = 0
    for algo, (dargv, cargv) in TOOLS.items():
        ran, failures = check_algorithm(algo, dargv, cargv)
        ran_any = ran_any or ran
        total_failures += failures

    print("-" * 60)
    if total_failures:
        print(f"RESULT: {total_failures} mismatch(es). FAIL.")
        return 1
    if not ran_any:
        print("RESULT: no CLI tools available — nothing cross-checked (skipped).")
        return 0
    print("RESULT: all available CLI tools round-tripped both ways. PASS.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
