#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "compress_utils.hpp"
#include "compress_utils_func.hpp"

namespace py = pybind11;
using namespace compress_utils;

#include <algorithm>
#include <cctype>
#include <string>

// Helper function to trim and convert a string to lowercase
std::string to_lower_trim(const std::string& str) {
    auto start = str.begin();
    auto end = str.end();
    while (start != end && std::isspace(*start)) ++start;
    while (start != end && std::isspace(*(end - 1))) --end;
    std::string result(start, end);
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

Algorithm parse_algorithm(const py::object& algorithm) {
    if (py::isinstance<py::str>(algorithm)) {
        std::string alg_str = to_lower_trim(algorithm.cast<std::string>());
#ifdef INCLUDE_BROTLI
        if (alg_str == "brotli") return Algorithm::BROTLI;
#endif
#ifdef INCLUDE_XZ
        if (alg_str == "xz") return Algorithm::XZ;
        if (alg_str == "lzma") return Algorithm::LZMA;
#endif
#ifdef INCLUDE_ZLIB
        if (alg_str == "zlib") return Algorithm::ZLIB;
#endif
#ifdef INCLUDE_ZSTD
        if (alg_str == "zstd") return Algorithm::ZSTD;
#endif
        throw std::invalid_argument("Unknown algorithm: " + alg_str);
    }
    else if (py::isinstance<py::int_>(algorithm)) {
        return static_cast<Algorithm>(algorithm.cast<int>());
    }
    throw std::invalid_argument("Algorithm must be a string or an Algorithm enum.");
}

PYBIND11_MODULE(compress_utils_py, m) {
    m.doc() = "Python bindings for compress-utils library";

    // Expose the Algorithm enum with pybind11
    py::enum_<Algorithm> py_algorithm(m, "Algorithm");

    py::dict members;

#ifdef INCLUDE_BROTLI
    py_algorithm.value("brotli", Algorithm::BROTLI);
    members["brotli"] = Algorithm::BROTLI;
#endif
#ifdef INCLUDE_XZ
    py_algorithm.value("lzma", Algorithm::LZMA);
    py_algorithm.value("xz", Algorithm::XZ);
    members["lzma"] = Algorithm::LZMA;
    members["xz"] = Algorithm::XZ;
#endif
#ifdef INCLUDE_ZLIB
    py_algorithm.value("zlib", Algorithm::ZLIB);
    members["zlib"] = Algorithm::ZLIB;
#endif
#ifdef INCLUDE_ZSTD
    py_algorithm.value("zstd", Algorithm::ZSTD);
    members["zstd"] = Algorithm::ZSTD;
#endif
    py_algorithm.export_values();

    // Define the __iter__ method to make the enum iterable
    py_algorithm.def("__iter__", [](py::object self) {
        return py::iter(self.attr("__members__").attr("values")());
    });

    // Compressor class (OOP Interface)
    py::class_<Compressor>(m, "compressor")
        .def(py::init([](const py::object& algorithm) {
            return new Compressor(parse_algorithm(algorithm));
        }), py::arg("algorithm"))
        .def("compress", [](Compressor& self, py::buffer data, int level = 3) {
            py::buffer_info info = data.request();
            const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
            size_t data_size = info.size * info.itemsize;

            std::vector<uint8_t> compressed_data = self.Compress(data_ptr, data_size, level);

            return py::bytes(reinterpret_cast<const char*>(compressed_data.data()), compressed_data.size());
        }, py::arg("data"), py::arg("level") = 3, "Compress data with optional level")
        .def("decompress", [](Compressor& self, py::buffer data) {
            py::buffer_info info = data.request();
            const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
            size_t data_size = info.size * info.itemsize;

            std::vector<uint8_t> decompressed_data = self.Decompress(data_ptr, data_size);

            return py::bytes(reinterpret_cast<const char*>(decompressed_data.data()), decompressed_data.size());
        }, py::arg("data"), "Decompress data");

    // Functional API: Compress and Decompress
    m.def("compress", [](py::buffer data, const py::object& algorithm, int level = 3) {
        py::buffer_info info = data.request();
        const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
        size_t data_size = info.size * info.itemsize;

        std::vector<uint8_t> compressed_data = Compress(data_ptr, data_size, parse_algorithm(algorithm), level);

        return py::bytes(reinterpret_cast<const char*>(compressed_data.data()), compressed_data.size());
    }, py::arg("data"), py::arg("algorithm"), py::arg("level") = 3, "Compress data using an algorithm and optional level");

    m.def("decompress", [](py::buffer data, const py::object& algorithm) {
        py::buffer_info info = data.request();
        const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
        size_t data_size = info.size * info.itemsize;

        std::vector<uint8_t> decompressed_data = Decompress(data_ptr, data_size, parse_algorithm(algorithm));

        return py::bytes(reinterpret_cast<const char*>(decompressed_data.data()), decompressed_data.size());
    }, py::arg("data"), py::arg("algorithm"), "Decompress data using an algorithm");
}
