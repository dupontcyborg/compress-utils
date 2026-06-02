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
"""

import unittest

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


REFERENCES = {
    "zstd":   _zstd_ref,
    "brotli": _brotli_ref,
    "zlib":   _zlib_ref,
    "bz2":    _bz2_ref,
    "lz4":    _lz4_ref,
    "xz":     _xz_ref,
}


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


if __name__ == "__main__":
    unittest.main()
