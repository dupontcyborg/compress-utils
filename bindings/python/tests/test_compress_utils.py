"""
Python smoke tests for compress_utils.

Mirrors the C and C++ smoke tests: round-trips every available algorithm
via the free functions and the streaming classes, plus a few edge cases
(empty, single byte, large), and exercises cross-API (stream-compress to
one-shot decompress and vice versa).
"""

import pathlib
import random
import subprocess
import sys
import sysconfig
import threading
import time
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


# Raw block formats skip the generic round-trip tests: lz4_raw has no streaming
# encoder and lzo has no encoder at all.
RAW_FORMATS = {"lz4_raw", "lzo"}

# Independent LZO1X block: five literals ("hello") then the end marker.
LZO_HELLO = bytes([22]) + b"hello" + bytes([17, 0, 0])


def available_algorithms():
    """Return names of framed algorithms compiled into this build, excluding lzma alias."""
    return [a.name for a in cu.Algorithm.__members__.values()
            if cu.is_available(a) and a.name != "lzma" and a.name not in RAW_FORMATS]


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

    def test_expected_size(self):
        data = bytes(range(256)) * 64  # 16KB
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                compressed = cu.compress(data, name)
                self.assertEqual(cu.decompress(compressed, name, expected_size=len(data)), data)
                self.assertEqual(cu.decompress(compressed, name, expected_size=len(data) + 4096), data)
                with self.assertRaises(cu.CompressError):
                    cu.decompress(compressed, name, expected_size=len(data) // 2)

    def test_concatenated_frames(self):
        # zstd/pzstd and gzip/pigz output can hold several frames or members.
        a, b = b"first part " * 50, b"second part " * 70
        for name in ("zstd", "gzip"):
            if not cu.is_available(name):
                continue
            with self.subTest(algorithm=name):
                joined = cu.compress(a, name) + cu.compress(b, name)
                self.assertEqual(cu.decompress(joined, name), a + b)
                ds = cu.DecompressStream(name)
                self.assertEqual(ds.decompress(joined) + ds.finish(), a + b)
                ds = cu.DecompressStream(name)
                half = len(joined) // 2
                self.assertEqual(ds.decompress(joined[:half]) + ds.decompress(joined[half:]) + ds.finish(), a + b)
        if cu.is_available("zlib"):
            with self.assertRaises(cu.CompressError):
                cu.decompress(cu.compress(a, "zlib") + cu.compress(b, "zlib"), "zlib")

    @unittest.skipUnless(cu.is_available("lz4_raw"), "lz4_raw not compiled in")
    def test_lz4_raw(self):
        for case_name, data in CASES.items():
            with self.subTest(case=case_name):
                block = cu.compress(data, "lz4_raw")
                self.assertEqual(cu.decompress(block, "lz4_raw", expected_size=len(data)), data)
                self.assertEqual(cu.decompress(block, cu.Algorithm.lz4_raw, expected_size=len(data) + 100), data)
        self.assertEqual(cu.decompress(cu.compress(SAMPLE_DATA, "lz4_raw"), "lz4_raw"), SAMPLE_DATA)
        with self.assertRaises(cu.CompressError):
            cu.decompress(cu.compress(SAMPLE_DATA, "lz4_raw"), "lz4_raw", expected_size=1)
        stream = cu.DecompressStream("lz4_raw")
        self.assertEqual(stream.decompress(cu.compress(SAMPLE_DATA, "lz4_raw")), SAMPLE_DATA)
        self.assertEqual(stream.finish(), b"")

    @unittest.skipUnless(cu.is_available("lzo"), "lzo not compiled in")
    def test_lzo(self):
        self.assertEqual(cu.decompress(LZO_HELLO, "lzo"), b"hello")
        self.assertEqual(cu.decompress(LZO_HELLO, cu.Algorithm.lzo, expected_size=64), b"hello")
        with self.assertRaises(cu.CompressError):
            cu.decompress(LZO_HELLO, "lzo", expected_size=4)
        with self.assertRaises(cu.CompressError):
            cu.compress(b"hello", "lzo")

    def test_is_available(self):
        for a in cu.Algorithm.__members__.values():
            self.assertIsInstance(cu.is_available(a), bool)

    def test_ships_type_information(self):
        # PEP 561: without py.typed, mypy ignores the stubs. cibuildwheel runs
        # this suite against each built wheel, so this checks what ships.
        package = pathlib.Path(cu.__file__).parent
        self.assertTrue((package / "py.typed").is_file())
        self.assertTrue((package / "compress_utils_py.pyi").is_file())


# The binding keeps the GIL for calls under 256 KiB, so these are larger.
GIL_PAYLOAD = bytes(random.Random(0).randbytes(4096)) * 4096  # 16 MiB, compressible
GIL_RANDOM = random.Random(1).randbytes(1024 * 1024)  # incompressible


class TestThreading(unittest.TestCase):
    @unittest.skipUnless(sysconfig.get_config_var("Py_GIL_DISABLED"), "not a free-threaded build")
    def test_import_keeps_the_gil_disabled(self):
        # On free-threaded Python, importing a module that doesn't declare
        # free-threading support turns the GIL back on. Check in a fresh
        # interpreter, since other tests may import libraries that do that.
        result = subprocess.run(
            [sys.executable, "-c", "import compress_utils, sys; print(sys._is_gil_enabled())"],
            capture_output=True, text=True, check=True,
        )
        self.assertEqual(result.stdout.strip(), "False", result.stderr)

    def test_large_calls_release_the_gil(self):
        # With a 10 s switch interval, a thread holding the GIL is never made
        # to give it up, so the helper can only count while a call has
        # released it. The input is small: the release comes from the 16 MiB
        # size recorded in the frame.
        packed = cu.compress(GIL_PAYLOAD, "zstd")
        self.assertLess(len(packed), 256 * 1024)
        count, go, stop = [0], threading.Event(), threading.Event()

        def helper():
            go.wait()
            while not stop.is_set():
                count[0] += 1
                time.sleep(0)  # releases the GIL so the caller can get it back

        thread = threading.Thread(target=helper)
        thread.start()
        old_interval = sys.getswitchinterval()
        sys.setswitchinterval(10)
        try:
            go.set()
            before = count[0]
            for _ in range(5):
                self.assertEqual(cu.decompress(packed, "zstd"), GIL_PAYLOAD)
            during = count[0] - before
        finally:
            sys.setswitchinterval(old_interval)
            stop.set()
            thread.join()
        self.assertGreater(during, 0)

    def test_input_cannot_be_resized_mid_call(self):
        # The binding keeps the buffer exported while it runs without the GIL,
        # so resizing a bytearray raises BufferError instead of freeing memory
        # being read. gzip ignores trailing zero padding, so an append that
        # lands between calls doesn't change what decodes. gzip doesn't record
        # its size, so the input itself must be large: use random data.
        data = bytearray(cu.compress(GIL_RANDOM, "gzip"))
        refused, done = [0], threading.Event()

        def resizer():
            while not done.is_set():
                try:
                    data.append(0)
                    data.pop()
                except BufferError:
                    refused[0] += 1

        thread = threading.Thread(target=resizer)
        thread.start()
        try:
            for _ in range(10):
                self.assertEqual(cu.decompress(data, "gzip"), GIL_RANDOM)
        finally:
            done.set()
            thread.join()
        self.assertGreater(refused[0], 0)

    def test_shared_stream_serializes_calls(self):
        # Every thread writes the same complete zstd frame to one stream, so in
        # any interleaving each call must decode to exactly that chunk;
        # overlapping calls on the stream would corrupt its state. Frames this
        # size reliably overlap (and fail) when the mutex is missing.
        chunk = GIL_RANDOM[:512 * 1024]
        frame = cu.compress(chunk, "zstd")
        stream = cu.DecompressStream("zstd")
        results, errors = [], []

        def writer():
            try:
                for _ in range(10):
                    results.append(stream.decompress(frame) == chunk)
            except Exception as e:
                errors.append(e)

        workers = [threading.Thread(target=writer) for _ in range(8)]
        for w in workers:
            w.start()
        for w in workers:
            w.join()
        self.assertEqual(errors, [])
        self.assertEqual(results, [True] * 80)
        self.assertEqual(stream.finish(), b"")

if __name__ == "__main__":
    unittest.main()
