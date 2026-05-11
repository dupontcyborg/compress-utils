"""
Python smoke tests for compress_utils.

Mirrors the C and C++ smoke tests: round-trips every available algorithm
via the free functions and the streaming classes, plus a few edge cases
(empty, single byte, large), and exercises cross-API (stream-compress to
one-shot decompress and vice versa).
"""

import random
import unittest

import compress_utils as cu


SAMPLE_DATA = b"Hello World"
EMPTY_DATA = b""
SINGLE_BYTE_DATA = b"A"
RANDOM_LARGE = bytes(random.randint(0, 255) for _ in range(1024 * 1024))
REPETITIVE_LARGE = b"A" * (1024 * 1024)

CASES = {
    "sample": SAMPLE_DATA,
    "empty": EMPTY_DATA,
    "single_byte": SINGLE_BYTE_DATA,
    "random_1MB": RANDOM_LARGE,
    "repetitive_1MB": REPETITIVE_LARGE,
}


def available_algorithms():
    """Return names of algorithms compiled into this build, excluding lzma alias."""
    return [a.name for a in cu.Algorithm.__members__.values()
            if cu.is_available(a) and a.name != "lzma"]


class TestCompressUtils(unittest.TestCase):

    def test_version(self):
        v = cu.version()
        self.assertRegex(v, r"^\d+\.\d+\.\d+$")

    def test_freefn_roundtrip(self):
        for name in available_algorithms():
            for case_name, data in CASES.items():
                with self.subTest(algorithm=name, case=case_name):
                    compressed = cu.compress(data, name, level=5)
                    if data:
                        self.assertTrue(len(compressed) > 0,
                                        f"{name} produced empty compressed output for non-empty input")
                    restored = cu.decompress(compressed, name)
                    self.assertEqual(restored, data)

    def test_freefn_string_or_enum(self):
        data = b"test payload"
        a = cu.compress(data, "zstd")
        b = cu.compress(data, cu.Algorithm.zstd)
        self.assertEqual(cu.decompress(a, "zstd"), data)
        self.assertEqual(cu.decompress(b, cu.Algorithm.zstd), data)

    def test_stream_roundtrip(self):
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                data = b"abcdefgh" * 8192  # 64KB
                cs = cu.CompressStream(name, level=5)
                pieces = []
                step = len(data) // 3 + 1
                for off in range(0, len(data), step):
                    pieces.append(cs.compress(data[off:off + step]))
                pieces.append(cs.finish())
                compressed = b"".join(pieces)

                ds = cu.DecompressStream(name)
                out_pieces = [ds.decompress(compressed), ds.finish()]
                self.assertEqual(b"".join(out_pieces), data)

    def test_cross_api(self):
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                data = bytes(range(256)) * 64  # 16KB
                cs = cu.CompressStream(name, 5)
                compressed_a = cs.compress(data) + cs.finish()
                self.assertEqual(cu.decompress(compressed_a, name), data)

                compressed_b = cu.compress(data, name, 5)
                ds = cu.DecompressStream(name)
                self.assertEqual(ds.decompress(compressed_b) + ds.finish(), data)

    def test_error_on_garbage(self):
        with self.assertRaises(cu.CompressError):
            cu.decompress(b"\xff" * 32, "zstd")

    def test_is_available(self):
        for a in cu.Algorithm.__members__.values():
            self.assertIsInstance(cu.is_available(a), bool)


if __name__ == "__main__":
    unittest.main()
