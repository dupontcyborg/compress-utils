"""
Python smoke tests for compress_utils.

Mirrors the C and C++ smoke tests: round-trips every available algorithm
via the free functions and the streaming classes, plus a few edge cases
(empty, single byte, large), and exercises cross-API (stream-compress to
one-shot decompress and vice versa).
"""

import io
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

    def test_output_far_larger_than_input(self):
        # Formats without a recorded size decode into a buffer that grows with
        # the output; 10 MB from a few KB of input doubles it many times, and a
        # buffer boundary must not lose or repeat a byte.
        words = b"alpha beta gamma delta epsilon zeta eta theta iota kappa\n"
        data = (words * 1000 + bytes(range(256)) * 40) * 150
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                compressed = cu.compress(data, name)
                self.assertGreater(len(data), 8 * len(compressed))
                self.assertEqual(cu.decompress(compressed, name), data)

    def test_max_decompressed_size_every_format(self):
        # The cap holds for formats that don't record their size too: those
        # decode through a stream that the cap also bounds.
        data = b"The quick brown fox jumps over the lazy dog. " * 1500
        for name in available_algorithms() + ["lz4_raw"]:
            with self.subTest(algorithm=name):
                compressed = cu.compress(data, name)
                try:
                    cu.set_max_decompressed_size(len(data) - 1)
                    with self.assertRaisesRegex(cu.CompressError, "limit|cap"):
                        cu.decompress(compressed, name)
                    cu.set_max_decompressed_size(len(data))
                    self.assertEqual(cu.decompress(compressed, name), data)
                    cu.set_max_decompressed_size(0)
                    self.assertEqual(cu.decompress(compressed, name), data)
                finally:
                    cu.set_max_decompressed_size(1 << 30)

    def test_stream_max_output(self):
        data = b"The quick brown fox jumps over the lazy dog. " * 1500
        for name in available_algorithms() + ["lz4_raw"]:
            with self.subTest(algorithm=name):
                compressed = cu.compress(data, name)
                ds = cu.DecompressStream(name, max_output=len(data) - 1)
                with self.assertRaisesRegex(cu.CompressError, "limit|cap"):
                    ds.decompress(compressed)
                    ds.finish()
                ds = cu.DecompressStream(name, max_output=len(data))
                self.assertEqual(ds.decompress(compressed) + ds.finish(), data)
        with self.assertRaises(TypeError):
            cu.DecompressStream("zstd", 10)  # keyword-only

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

    def test_flush(self):
        # flush() delivers everything so far without ending the stream.
        messages = [b"A" * 131072, bytes(range(256)) * 300, b"B" * 37]
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                cs = cu.CompressStream(name)
                if name in ("snappy", "bz2"):
                    cs.compress(b"abc")
                    with self.assertRaises(cu.CompressError):
                        cs.flush()
                    continue
                ds = cu.DecompressStream(name)
                sent = received = b""
                for message in messages:
                    wire = cs.compress(message) + cs.flush()
                    sent += message
                    received += ds.decompress(wire)
                    self.assertEqual(received, sent)
                    self.assertEqual(cs.flush(), b"")  # nothing new to flush
                received += ds.decompress(cs.finish()) + ds.finish()
                self.assertEqual(received, sent)

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
# What follows a zpickle message: the next message's header, then data.
TRAILER = b"ZPKL\x01\x01\x03\x00" + b"\x00" * 64


class TestSingleStream(unittest.TestCase):
    """DecompressStream(single_stream=True): eof / unused_data, as in bz2/lzma."""

    def feed(self, ds, data, pieces):
        """Feeds `data` cut at `pieces` until eof; returns (output, bytes fed)."""
        out, fed, start = b"", 0, 0
        for end in list(pieces) + [len(data)]:
            if ds.eof or start >= len(data):
                break
            out += ds.decompress(data[start:end])
            fed, start = end, end
        return out, fed

    def test_trailing_bytes(self):
        for name in available_algorithms():
            for payload in (EMPTY_DATA, SAMPLE_DATA, RANDOM_LARGE[:200000]):
                frame = cu.compress(payload, name)
                data = frame + TRAILER
                n = len(frame)
                cuts = {"whole": [], "at end": [n], "end-1": [n - 1], "end+1": [n + 1]}
                if len(payload) <= len(SAMPLE_DATA):
                    cuts["byte by byte"] = range(1, len(data))
                for label, pieces in cuts.items():
                    with self.subTest(algorithm=name, size=len(payload), cut=label):
                        ds = cu.DecompressStream(name, single_stream=True)
                        out, fed = self.feed(ds, data, pieces)
                        self.assertEqual(out, payload)
                        self.assertTrue(ds.eof)
                        self.assertEqual(ds.unused_data, data[n:fed])

    def test_two_frames(self):
        a, b = b"first message " * 300, RANDOM_LARGE[:5000]
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                ds = cu.DecompressStream(name, single_stream=True)
                self.assertEqual(ds.decompress(cu.compress(a, name) + cu.compress(b, name)), a)
                self.assertTrue(ds.eof)
                second = cu.DecompressStream(name, single_stream=True)
                self.assertEqual(second.decompress(ds.unused_data), b)
                self.assertTrue(second.eof)
                self.assertEqual(second.unused_data, b"")

    def test_after_end(self):
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                ds = cu.DecompressStream(name, single_stream=True)
                ds.decompress(cu.compress(SAMPLE_DATA, name) + b"rest")
                with self.assertRaises(EOFError):
                    ds.decompress(b"more")
                with self.assertRaises(cu.CompressError) as raised:
                    ds.decompress(b"more")
                self.assertIsInstance(raised.exception, cu.EndOfStreamError)
                self.assertEqual(ds.unused_data, b"rest")
                self.assertEqual(ds.decompress(b""), b"")
                self.assertEqual(ds.finish(), b"")
                self.assertEqual(ds.finish(), b"")
                with self.assertRaises(cu.EndOfStreamError):
                    ds.decompress(b"more")
                self.assertEqual(ds.unused_data, b"rest")

    def test_truncated(self):
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                frame = cu.compress(RANDOM_LARGE[:1000], name)
                ds = cu.DecompressStream(name, single_stream=True)
                ds.decompress(frame[:-1])
                with self.assertRaises(cu.CompressError):
                    ds.finish()
                self.assertFalse(ds.eof)
                self.assertEqual(ds.unused_data, b"")

    def test_default_mode_unchanged(self):
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                ds = cu.DecompressStream(name)
                self.assertEqual(ds.decompress(cu.compress(SAMPLE_DATA, name)) + ds.finish(),
                                 SAMPLE_DATA)
                self.assertFalse(ds.eof)
                self.assertEqual(ds.unused_data, b"")
                # A write after finish() is still a plain CompressError.
                with self.assertRaises(cu.CompressError) as raised:
                    ds.decompress(b"more")
                self.assertNotIsInstance(raised.exception, EOFError)

    def test_keyword_only_and_unsupported(self):
        with self.assertRaises(TypeError):
            cu.DecompressStream("zstd", True)
        for name in RAW_FORMATS:
            if cu.is_available(name):
                with self.subTest(algorithm=name), self.assertRaises(cu.CompressError):
                    cu.DecompressStream(name, single_stream=True)

    def test_reader_continues_after_the_end(self):
        # The zpickle pattern: messages back to back in a file, read in 64 KiB
        # chunks; after each message, seek back over the unused bytes.
        messages = [RANDOM_LARGE[:150000], b"", b"small", REPETITIVE_LARGE[:300000]]
        for name in available_algorithms():
            with self.subTest(algorithm=name):
                f = io.BytesIO(b"".join(cu.compress(m, name) for m in messages) + b"tail")
                for message in messages:
                    ds = cu.DecompressStream(name, single_stream=True)
                    out = b""
                    while not ds.eof:
                        chunk = f.read(65536)
                        self.assertTrue(chunk, "ran out of input before the end")
                        out += ds.decompress(chunk)
                    f.seek(-len(ds.unused_data), io.SEEK_CUR)
                    self.assertEqual(out, message)
                self.assertEqual(f.read(), b"tail")


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

    def test_single_stream_properties_while_decoding(self):
        # eof and unused_data take the stream's lock, so reading them from
        # other threads while a large decode runs sees either the state before
        # the call or after it, never a torn one. The two reads are separate
        # calls, so the decode can finish between them, but once eof reads
        # True, unused_data is already complete.
        frame = cu.compress(GIL_PAYLOAD, "zstd")
        stream = cu.DecompressStream("zstd", single_stream=True)
        seen, errors, done = set(), [], threading.Event()

        def reader():
            try:
                while not done.is_set():
                    eof = stream.eof
                    unused = stream.unused_data
                    if eof and unused != b"next":
                        errors.append(f"eof but unused_data is {unused!r}")
                    seen.add(unused)
            except Exception as e:
                errors.append(e)

        readers = [threading.Thread(target=reader) for _ in range(4)]
        for r in readers:
            r.start()
        try:
            self.assertEqual(stream.decompress(frame + b"next"), GIL_PAYLOAD)
        finally:
            done.set()
            for r in readers:
                r.join()
        self.assertEqual(errors, [])
        self.assertLessEqual(seen, {b"", b"next"})
        self.assertEqual((stream.eof, stream.unused_data), (True, b"next"))

if __name__ == "__main__":
    unittest.main()
