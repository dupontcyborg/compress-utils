/*
 * compress_utils_py.cpp — pybind11 binding for compress-utils.
 *
 * Thin wrapper over the header-only C++ binding (compress_utils.hpp),
 * which in turn calls the C ABI. No state of its own.
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
        if (s == "bz2"  || s == "bzip2")       return cu::Algorithm::Bz2;
        if (s == "lz4")                        return cu::Algorithm::Lz4;
        if (s == "xz")                         return cu::Algorithm::Xz;
        if (s == "lzma")                       return cu::Algorithm::Lzma;
        if (s == "snappy")                     return cu::Algorithm::Snappy;
        throw std::invalid_argument("Unknown algorithm: " + s);
    }
    /* Accept the Algorithm enum and bare int. */
    return obj.cast<cu::Algorithm>();
}

/* ---- Buffer adapters ---- */

static std::span<const std::uint8_t> as_span(py::buffer data) {
    py::buffer_info info = data.request();
    return std::span<const std::uint8_t>(
        static_cast<const std::uint8_t*>(info.ptr),
        static_cast<std::size_t>(info.size * info.itemsize)
    );
}

static py::bytes to_bytes(const std::vector<std::uint8_t>& v) {
    return py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
}

/* ---- Module ---- */

PYBIND11_MODULE(compress_utils_py, m) {
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
        .export_values();

    m.def("version",      &cu::version);
    m.def("is_available", [](const py::object& algo) {
        return cu::is_available(parse_algorithm(algo));
    }, py::arg("algorithm"));
    m.def("set_max_decompressed_size", &cu::set_max_decompressed_size,
          py::arg("bytes"));

    /* Functional API. */
    m.def("compress", [](py::buffer data, const py::object& algorithm, int level) {
        return to_bytes(cu::compress(parse_algorithm(algorithm), as_span(data), level));
    }, py::arg("data"), py::arg("algorithm"), py::arg("level") = 5,
       "Compress bytes/buffer using the given algorithm (string or Algorithm).");

    m.def("decompress", [](py::buffer data, const py::object& algorithm) {
        return to_bytes(cu::decompress(parse_algorithm(algorithm), as_span(data)));
    }, py::arg("data"), py::arg("algorithm"),
       "Decompress bytes/buffer using the given algorithm.");

    /* Streaming. */
    py::class_<cu::CompressStream>(m, "CompressStream",
        "Streaming compression. Feed chunks via .compress(b); flush with .finish().")
        .def(py::init([](const py::object& algorithm, int level) {
            return new cu::CompressStream(parse_algorithm(algorithm), level);
        }), py::arg("algorithm"), py::arg("level") = 5)
        .def("compress", [](cu::CompressStream& self, py::buffer data) {
            return to_bytes(self.write(as_span(data)));
        }, py::arg("data"))
        .def("finish", [](cu::CompressStream& self) {
            return to_bytes(self.finish());
        });

    py::class_<cu::DecompressStream>(m, "DecompressStream",
        "Streaming decompression. Feed chunks via .decompress(b); flush with .finish().")
        .def(py::init([](const py::object& algorithm) {
            return new cu::DecompressStream(parse_algorithm(algorithm));
        }), py::arg("algorithm"))
        .def("decompress", [](cu::DecompressStream& self, py::buffer data) {
            return to_bytes(self.write(as_span(data)));
        }, py::arg("data"))
        .def("finish", [](cu::DecompressStream& self) {
            return to_bytes(self.finish());
        });

    /* Translate cu::Error to a Python exception. */
    static py::exception<cu::Error> cu_error_exc(m, "CompressError");
    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const cu::Error& e) {
            PyErr_SetString(cu_error_exc.ptr(), e.what());
        }
    });
}
