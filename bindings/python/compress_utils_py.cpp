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

/* A small input can still decode to a lot, so also ask the format for its
 * recorded output size when it has one (zstd, LZ4 frames, Snappy). */
static bool worth_releasing_decompress(cu::Algorithm algo, std::span<const std::uint8_t> in) {
    if (worth_releasing(in.size())) return true;
    std::size_t hint = 0;
    return cu_decompress_size_hint(static_cast<cu_algorithm_t>(algo), in.data(), in.size(),
                                   &hint) == CU_OK &&
           worth_releasing(hint);
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
static std::vector<std::uint8_t> run_locked(Locked<Stream>& self, bool release, F&& f) {
    if (!release) {
        std::unique_lock<std::mutex> lock(self.mutex, std::try_to_lock);
        if (lock.owns_lock()) return f(self.stream);
    }
    py::gil_scoped_release nogil;
    std::lock_guard<std::mutex> lock(self.mutex);
    return f(self.stream);
}

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
        bool release = expected_size ? worth_releasing(in.span().size()) ||
                                           worth_releasing(*expected_size)
                                     : worth_releasing_decompress(algo, in.span());
        auto out = run(release, [&] {
            return expected_size ? cu::decompress(algo, in.span(), *expected_size)
                                 : cu::decompress(algo, in.span());
        });
        return to_bytes(out);
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
        "Safe to share between threads: calls on one stream run one at a time.")
        .def(py::init([](const py::object& algorithm) {
            return new PyDecompressStream(parse_algorithm(algorithm));
        }), py::arg("algorithm"))
        .def("decompress", [](PyDecompressStream& self, py::buffer data) {
            InputView in(data);
            auto out = run_locked(self, worth_releasing(in.span().size()),
                                  [&](cu::DecompressStream& s) { return s.write(in.span()); });
            return to_bytes(out);
        }, py::arg("data"))
        .def("finish", [](PyDecompressStream& self) {
            return to_bytes(run_locked(self, true, [](cu::DecompressStream& s) { return s.finish(); }));
        });

    /* Translate cu::Error to a Python exception. register_exception stores the
     * exception type safely for free-threaded builds. */
    py::register_exception<cu::Error>(m, "CompressError");
}
