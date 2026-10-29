/*
 * compress_utils_py.cpp — pybind11 binding for compress-utils.
 *
 * Thin wrapper over the header-only C++ binding (compress_utils.hpp),
 * which in turn calls the C ABI. Its only state is a lock per stream.
 *
 * Calls on large inputs release the GIL so other Python threads keep running
 * while the codec works. Stream objects serialize their calls with a mutex,
 * so sharing one between threads can't corrupt it (as with stdlib zlib, bz2
 * and lzma objects). That also makes the module safe without a GIL, so it
 * declares free-threading support.
 *
 * Module name: compress_utils_py. Imported by the compress_utils package's
 * __init__.py.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <compress_utils.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace py = pybind11;

/* ---- Algorithm resolution from string or enum value ---- */

static std::string lower_trim(const std::string& s) {
    auto a = s.begin();
    auto b = s.end();
    while (a != b && std::isspace(*a)) ++a;
    while (a != b && std::isspace(*(b - 1))) --b;
    std::string out(a, b);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

static cu::Algorithm parse_algorithm(const py::object& obj) {
    if (py::isinstance<py::str>(obj)) {
        std::string s = lower_trim(obj.cast<std::string>());
        if (s == "zstd")                       return cu::Algorithm::Zstd;
        if (s == "brotli")                     return cu::Algorithm::Brotli;
        if (s == "zlib")                       return cu::Algorithm::Zlib;
        if (s == "gzip")                       return cu::Algorithm::Gzip;
        if (s == "deflate")                    return cu::Algorithm::Deflate;
        if (s == "bz2"  || s == "bzip2")       return cu::Algorithm::Bz2;
        if (s == "lz4")                        return cu::Algorithm::Lz4;
        if (s == "xz")                         return cu::Algorithm::Xz;
        if (s == "lzma")                       return cu::Algorithm::Lzma;
        if (s == "snappy")                     return cu::Algorithm::Snappy;
        if (s == "lz4_raw")                    return cu::Algorithm::Lz4Raw;
        if (s == "lzo")                        return cu::Algorithm::Lzo;
        throw std::invalid_argument("Unknown algorithm: " + s);
    }
    /* Accept the Algorithm enum and bare int. */
    return obj.cast<cu::Algorithm>();
}

/* ---- Buffer adapters ---- */

/*
 * A Python buffer held for the length of a call. Holding the export means its
 * owner can't resize or free it (a bytearray resize raises BufferError) while
 * the call runs without the GIL. Create and destroy it with the GIL held.
 */
class InputView {
public:
    explicit InputView(const py::buffer& data) : info_(data.request()) {}
    std::span<const std::uint8_t> span() const {
        return std::span<const std::uint8_t>(
            static_cast<const std::uint8_t*>(info_.ptr),
            static_cast<std::size_t>(info_.size * info_.itemsize)
        );
    }
private:
    py::buffer_info info_;
};

static py::bytes to_bytes(const std::vector<std::uint8_t>& v) {
    return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
}

/* ---- GIL release ---- */

/*
 * Calls smaller than this keep the GIL. Getting it back after releasing it
 * can take up to the interpreter's switch interval (5 ms by default) when
 * another thread is running Python, which would multiply the cost of a short
 * call. Overridable at build time for benchmarking.
 */
#ifndef CU_PY_RELEASE_GIL_MIN
#define CU_PY_RELEASE_GIL_MIN (256 * 1024)
#endif

static bool worth_releasing(std::size_t bytes) {
    return bytes >= CU_PY_RELEASE_GIL_MIN;
}

/* ---- One-shot decompress ---- */

/* A bytes object of `n` bytes whose contents are not yet set. Fill it before
 * the object is shared; nothing else can see it until it's returned. */
static py::bytes new_bytes(std::size_t n) {
    PyObject* o = PyBytes_FromStringAndSize(nullptr, static_cast<Py_ssize_t>(n));
    if (!o) throw py::error_already_set();
    return py::reinterpret_steal<py::bytes>(o);
}

/*
 * Decodes into a bytes object of `capacity` bytes, the most the output can be,
 * so the result needs no copy and no zero-fill. Shorter output (an
 * `expected_size` that is only a bound) is copied into a right-sized object.
 */
static py::bytes decompress_into_bytes(cu_algorithm_t algo, std::span<const std::uint8_t> in,
                                       std::size_t capacity, bool release) {
    py::bytes out = new_bytes(capacity);
    std::uint8_t empty = 0;  /* a non-null target for a zero capacity */
    auto* buf = capacity ? reinterpret_cast<std::uint8_t*>(PyBytes_AS_STRING(out.ptr())) : &empty;
    std::size_t out_len = capacity;
    {
        std::optional<py::gil_scoped_release> nogil;
        if (release) nogil.emplace();
        cu::detail::check(cu_decompress(algo, in.data(), in.size(), buf, &out_len));
    }
    if (out_len == capacity) return out;
    return py::bytes(reinterpret_cast<const char*>(buf), out_len);
}

/* A buffer from cu_decompress_alloc, released with cu_buffer_free. */
struct CoreBuffer {
    std::uint8_t* p = nullptr;
    std::size_t n = 0;
    ~CoreBuffer() { cu_buffer_free(p); }
};

/*
 * For a format that doesn't record its size: the core decodes into a buffer it grows (and bounds by the same cap as the sized path), then the result is copied once into a bytes object. Runs without the GIL when `release` is set.
 */
static py::bytes decompress_alloc(cu_algorithm_t algo, std::span<const std::uint8_t> in, bool release) {
    CoreBuffer buf;
    {
        std::optional<py::gil_scoped_release> nogil;
        if (release) nogil.emplace();
        cu::detail::check(cu_decompress_alloc(algo, in.data(), in.size(), &buf.p, &buf.n));
    }
    return py::bytes(reinterpret_cast<const char*>(buf.p), buf.n);
}

/* The size from `expected_size`, else the format's recorded size, else the core's growing decode. */
static py::bytes decompress_oneshot(cu::Algorithm a, std::span<const std::uint8_t> in,
                                    std::optional<std::size_t> expected_size) {
    cu_algorithm_t algo = cu::detail::c_algo(a);
    if (expected_size) {
        bool release = worth_releasing(in.size()) || worth_releasing(*expected_size);
        return decompress_into_bytes(algo, in, *expected_size, release);
    }
    std::size_t hint = 0;
    cu_status_t s = cu_decompress_size_hint(algo, in.data(), in.size(), &hint);
    if (s == CU_OK) {
        return decompress_into_bytes(algo, in, hint, worth_releasing(in.size()) || worth_releasing(hint));
    }
    if (s != CU_ERR_SIZE_UNKNOWN) cu::detail::throw_status(s);
    /* A small input can still decode to a lot, but with no recorded size there
     * is no way to tell, so only the input's size counts. */
    return decompress_alloc(algo, in, worth_releasing(in.size()));
}

/* Runs `f` without the GIL when `release` is set. `f` must not touch Python. */
template <typename F>
static std::vector<std::uint8_t> run(bool release, F&& f) {
    if (!release) return f();
    py::gil_scoped_release nogil;
    return f();
}

/* A stream and the mutex that serializes calls on it. */
template <typename Stream>
struct Locked {
    template <typename... Args>
    explicit Locked(Args&&... args) : stream(std::forward<Args>(args)...) {}
    Stream stream;
    std::mutex mutex;
};

/*
 * Runs `f(stream)` under the stream's mutex. A small call keeps the GIL if the
 * mutex is free. Otherwise the GIL is released before taking the mutex and
 * re-acquired after dropping it, so no thread ever waits for one while
 * holding the other.
 */
template <typename Stream, typename F>
static auto run_locked(Locked<Stream>& self, bool release, F&& f) -> decltype(f(self.stream)) {
    if (!release) {
        std::unique_lock<std::mutex> lock(self.mutex, std::try_to_lock);
        if (lock.owns_lock()) return f(self.stream);
    }
    py::gil_scoped_release nogil;
    std::lock_guard<std::mutex> lock(self.mutex);
    return f(self.stream);
}

/* Data passed to a single-stream DecompressStream after its end. Raised as
 * EndOfStreamError, which is both a CompressError and an EOFError. */
class EndOfStream : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

/* ---- Module ---- */

PYBIND11_MODULE(compress_utils_py, m, py::mod_gil_not_used()) {
    m.doc() = "Python bindings for compress-utils library (C-core).";

    /* Algorithm enum — lowercase names match the legacy Python API. */
    py::enum_<cu::Algorithm>(m, "Algorithm")
        .value("zstd",   cu::Algorithm::Zstd)
        .value("brotli", cu::Algorithm::Brotli)
        .value("zlib",   cu::Algorithm::Zlib)
        .value("bz2",    cu::Algorithm::Bz2)
        .value("lz4",    cu::Algorithm::Lz4)
        .value("xz",     cu::Algorithm::Xz)
        .value("lzma",   cu::Algorithm::Lzma)
        .value("snappy", cu::Algorithm::Snappy)
        .value("gzip",   cu::Algorithm::Gzip)
        .value("deflate", cu::Algorithm::Deflate)
        .value("lz4_raw", cu::Algorithm::Lz4Raw)
        .value("lzo",     cu::Algorithm::Lzo)
        .export_values();

    m.def("version",      &cu::version);
    m.def("is_available", [](const py::object& algo) {
        return cu::is_available(parse_algorithm(algo));
    }, py::arg("algorithm"));
    m.def("set_max_decompressed_size", &cu::set_max_decompressed_size,
          py::arg("bytes"));

    /* Functional API. */
    m.def("compress", [](py::buffer data, const py::object& algorithm, int level) {
        cu::Algorithm algo = parse_algorithm(algorithm);
        InputView in(data);
        auto out = run(worth_releasing(in.span().size()), [&] {
            return cu::compress(algo, in.span(), level);
        });
        return to_bytes(out);
    }, py::arg("data"), py::arg("algorithm"), py::arg("level") = 5,
       "Compress bytes/buffer using the given algorithm (string or Algorithm).");

    m.def("decompress", [](py::buffer data, const py::object& algorithm,
                           std::optional<std::size_t> expected_size) {
        cu::Algorithm algo = parse_algorithm(algorithm);
        InputView in(data);
        return decompress_oneshot(algo, in.span(), expected_size);
    }, py::arg("data"), py::arg("algorithm"), py::arg("expected_size") = py::none(),
       "Decompress bytes/buffer using the given algorithm.\n\n"
       "expected_size is an upper bound on the output size; the result holds the\n"
       "bytes actually decoded. It skips the size probe, and for lz4_raw it skips\n"
       "the much slower streaming fallback.");

    /* Streaming. flush() and finish() always release the GIL: they can
     * compress a lot of buffered work (e.g. a whole Snappy stream). */
    using PyCompressStream = Locked<cu::CompressStream>;
    py::class_<PyCompressStream>(m, "CompressStream",
        "Streaming compression. Feed chunks via .compress(b); .flush() delivers\n"
        "everything so far and keeps the stream open; .finish() ends it.\n"
        "Safe to share between threads: calls on one stream run one at a time.")
        .def(py::init([](const py::object& algorithm, int level) {
            return new PyCompressStream(parse_algorithm(algorithm), level);
        }), py::arg("algorithm"), py::arg("level") = 5)
        .def("compress", [](PyCompressStream& self, py::buffer data) {
            InputView in(data);
            auto out = run_locked(self, worth_releasing(in.span().size()),
                                  [&](cu::CompressStream& s) { return s.write(in.span()); });
            return to_bytes(out);
        }, py::arg("data"))
        .def("flush", [](PyCompressStream& self) {
            return to_bytes(run_locked(self, true, [](cu::CompressStream& s) { return s.flush(); }));
        },
             "Return the bytes a decoder needs to recover everything compressed so far,\n"
             "keeping the stream open for more data. Unlike zlib's flush(), this does\n"
             "not end the stream; use finish() for that. Each flush costs a few bytes.\n"
             "Raises CompressError for snappy and bz2, which can't flush mid-stream.")
        .def("finish", [](PyCompressStream& self) {
            return to_bytes(run_locked(self, true, [](cu::CompressStream& s) { return s.finish(); }));
        });

    using PyDecompressStream = Locked<cu::DecompressStream>;
    py::class_<PyDecompressStream>(m, "DecompressStream",
        "Streaming decompression. Feed chunks via .decompress(b); end with .finish().\n"
        "With single_stream=True, decoding stops at the end of the first frame:\n"
        ".eof turns True and .unused_data holds the input that came after it, as\n"
        "with bz2.BZ2Decompressor. Concatenated frames otherwise decode as one.\n"
        "max_output limits the total decompressed output in bytes: the call that\n"
        "would go past it raises CompressError and the stream can't be used after.\n"
        "Safe to share between threads: calls on one stream run one at a time.")
        .def(py::init([](const py::object& algorithm, bool single_stream,
                         std::optional<std::size_t> max_output) {
            auto* self = new PyDecompressStream(parse_algorithm(algorithm),
                                                single_stream ? cu::StreamMode::Single
                                                              : cu::StreamMode::Concatenated);
            if (max_output) self->stream.set_max_output(*max_output);
            return self;
        }), py::arg("algorithm"), py::kw_only(), py::arg("single_stream") = false,
            py::arg("max_output") = py::none())
        .def("decompress", [](PyDecompressStream& self, py::buffer data) {
            InputView in(data);
            auto out = run_locked(self, worth_releasing(in.span().size()),
                                  [&](cu::DecompressStream& s) {
                /* Checked under the lock, before the C++ stream would report
                 * this as an ordinary CompressError. */
                if (!in.span().empty() && s.eof()) {
                    throw EndOfStream("end of stream already reached; the data was not "
                                      "decompressed");
                }
                return s.write(in.span());
            });
            return to_bytes(out);
        }, py::arg("data"),
           "Decompress a chunk. With single_stream=True, raises EndOfStreamError once\n"
           ".eof is True (the data is not consumed; .unused_data is unchanged).")
        .def("finish", [](PyDecompressStream& self) {
            return to_bytes(run_locked(self, true, [](cu::DecompressStream& s) { return s.finish(); }));
        })
        .def_property_readonly("eof", [](PyDecompressStream& self) {
            return run_locked(self, false, [](cu::DecompressStream& s) { return s.eof(); });
        }, "True once a single_stream decoder has reached the end of the stream and\n"
           "returned all of its output. Always False otherwise.")
        .def_property_readonly("unused_data", [](PyDecompressStream& self) {
            auto unused = run_locked(self, false, [](cu::DecompressStream& s) {
                auto u = s.unused();
                return std::vector<std::uint8_t>(u.begin(), u.end());
            });
            return to_bytes(unused);
        }, "Input passed to a single_stream decoder after the end of the stream.\n"
           "Empty until .eof is True, and always empty otherwise.");

    /* Translate cu::Error to a Python exception. register_exception stores the
     * exception type safely for free-threaded builds. EndOfStreamError takes
     * both bases, so `except EOFError` works as with the stdlib decompressors
     * while `except CompressError` still catches every library error. */
    auto& compress_error = py::register_exception<cu::Error>(m, "CompressError");
    py::register_exception<EndOfStream>(
        m, "EndOfStreamError", py::make_tuple(compress_error, py::handle(PyExc_EOFError)));
}
