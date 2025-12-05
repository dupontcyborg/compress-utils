#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "compress_utils.hpp"
#include "compress_utils_func.hpp"
#include "compress_utils_stream.hpp"

namespace py = pybind11;

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

compress_utils::Algorithm parse_algorithm(const py::object& algorithm) {
    if (py::isinstance<py::str>(algorithm)) {
        std::string alg_str = to_lower_trim(algorithm.cast<std::string>());
#ifdef INCLUDE_BROTLI
        if (alg_str == "brotli") return compress_utils::Algorithm::BROTLI;
#endif
#ifdef INCLUDE_BZ2
        if (alg_str == "bz2" || alg_str == "bzip2") return compress_utils::Algorithm::BZ2;
#endif
#ifdef INCLUDE_LZ4
        if (alg_str == "lz4") return compress_utils::Algorithm::LZ4;
#endif
#ifdef INCLUDE_XZ
        if (alg_str == "xz") return compress_utils::Algorithm::XZ;
        if (alg_str == "lzma") return compress_utils::Algorithm::LZMA;
#endif
#ifdef INCLUDE_ZLIB
        if (alg_str == "zlib") return compress_utils::Algorithm::ZLIB;
#endif
#ifdef INCLUDE_ZSTD
        if (alg_str == "zstd") return compress_utils::Algorithm::ZSTD;
#endif
        throw std::invalid_argument("Unknown algorithm: " + alg_str);
    }
    else if (py::isinstance<py::int_>(algorithm)) {
        return static_cast<compress_utils::Algorithm>(algorithm.cast<int>());
    }
    throw std::invalid_argument("Algorithm must be a string or an Algorithm enum.");
}

PYBIND11_MODULE(compress_utils_py, m) {
    m.doc() = "Python bindings for compress-utils library";

    // Expose the Algorithm enum with pybind11
    py::enum_<compress_utils::Algorithm> py_algorithm(m, "Algorithm");

    py::dict members;

#ifdef INCLUDE_BROTLI
    py_algorithm.value("brotli", compress_utils::Algorithm::BROTLI);
    members["brotli"] = compress_utils::Algorithm::BROTLI;
#endif
#ifdef INCLUDE_BZ2
    py_algorithm.value("bz2", compress_utils::Algorithm::BZ2);
    members["bz2"] = compress_utils::Algorithm::BZ2;
#endif
#ifdef INCLUDE_LZ4
    py_algorithm.value("lz4", compress_utils::Algorithm::LZ4);
    members["lz4"] = compress_utils::Algorithm::LZ4;
#endif
#ifdef INCLUDE_XZ
    py_algorithm.value("lzma", compress_utils::Algorithm::LZMA);
    py_algorithm.value("xz", compress_utils::Algorithm::XZ);
    members["lzma"] = compress_utils::Algorithm::LZMA;
    members["xz"] = compress_utils::Algorithm::XZ;
#endif
#ifdef INCLUDE_ZLIB
    py_algorithm.value("zlib", compress_utils::Algorithm::ZLIB);
    members["zlib"] = compress_utils::Algorithm::ZLIB;
#endif
#ifdef INCLUDE_ZSTD
    py_algorithm.value("zstd", compress_utils::Algorithm::ZSTD);
    members["zstd"] = compress_utils::Algorithm::ZSTD;
#endif
    py_algorithm.export_values();

    // Define the __iter__ method to make the enum iterable
    py_algorithm.def("__iter__", [](py::object self) {
        return py::iter(self.attr("__members__").attr("values")());
    });

    // Compressor class (OOP Interface)
    py::class_<compress_utils::Compressor>(m, "compressor")
        .def(py::init([](const py::object& algorithm) {
            return new compress_utils::Compressor(parse_algorithm(algorithm));
        }), py::arg("algorithm"))
        .def("compress", [](compress_utils::Compressor& self, py::buffer data, int level = 3) {
            py::buffer_info info = data.request();
            const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
            size_t data_size = info.size * info.itemsize;

            std::vector<uint8_t> compressed_data = self.Compress(data_ptr, data_size, level);

            return py::bytes(reinterpret_cast<const char*>(compressed_data.data()), compressed_data.size());
        }, py::arg("data"), py::arg("level") = 3, "Compress data with optional level")
        .def("decompress", [](compress_utils::Compressor& self, py::buffer data) {
            py::buffer_info info = data.request();
            const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
            size_t data_size = info.size * info.itemsize;

            std::vector<uint8_t> decompressed_data = self.Decompress(data_ptr, data_size);

            return py::bytes(reinterpret_cast<const char*>(decompressed_data.data()), decompressed_data.size());
        }, py::arg("data"), "Decompress data");

    // CompressStream class (Streaming Interface)
    py::class_<compress_utils::CompressStream>(m, "CompressStream",
        R"doc(
        Streaming compression class for incremental data compression.

        This class allows compressing data in chunks without loading the entire
        dataset into memory. Useful for large files or streaming data.

        Example:
            stream = CompressStream('zstd', level=5)
            while has_more_data:
                compressed = stream.compress(chunk)
                output.write(compressed)
            output.write(stream.finish())
        )doc")
        .def(py::init([](const py::object& algorithm, int level) {
            return new compress_utils::CompressStream(parse_algorithm(algorithm), level);
        }), py::arg("algorithm"), py::arg("level") = 3,
        "Create a new compression stream with the specified algorithm and level")
        .def("compress", [](compress_utils::CompressStream& self, py::buffer data) {
            py::buffer_info info = data.request();
            const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
            size_t data_size = info.size * info.itemsize;

            std::span<const uint8_t> span(data_ptr, data_size);
            std::vector<uint8_t> result = self.Compress(span);

            return py::bytes(reinterpret_cast<const char*>(result.data()), result.size());
        }, py::arg("data"),
        "Compress a chunk of data. Returns compressed output (may be empty if buffered).")
        .def("finish", [](compress_utils::CompressStream& self) {
            std::vector<uint8_t> result = self.Finish();
            return py::bytes(reinterpret_cast<const char*>(result.data()), result.size());
        },
        "Finish compression and return any remaining compressed data.")
        .def("is_finished", &compress_utils::CompressStream::IsFinished,
        "Check if the stream has been finished.")
        .def_property_readonly("algorithm", [](compress_utils::CompressStream& self) {
            return self.algorithm();
        }, "Get the compression algorithm being used.");

    // DecompressStream class (Streaming Interface)
    py::class_<compress_utils::DecompressStream>(m, "DecompressStream",
        R"doc(
        Streaming decompression class for incremental data decompression.

        This class allows decompressing data in chunks without loading the entire
        compressed dataset into memory.

        Example:
            stream = DecompressStream('zstd')
            while has_more_data:
                decompressed = stream.decompress(chunk)
                output.write(decompressed)
            stream.finish()
        )doc")
        .def(py::init([](const py::object& algorithm) {
            return new compress_utils::DecompressStream(parse_algorithm(algorithm));
        }), py::arg("algorithm"),
        "Create a new decompression stream with the specified algorithm")
        .def("decompress", [](compress_utils::DecompressStream& self, py::buffer data) {
            py::buffer_info info = data.request();
            const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
            size_t data_size = info.size * info.itemsize;

            std::span<const uint8_t> span(data_ptr, data_size);
            std::vector<uint8_t> result = self.Decompress(span);

            return py::bytes(reinterpret_cast<const char*>(result.data()), result.size());
        }, py::arg("data"),
        "Decompress a chunk of data. Returns decompressed output.")
        .def("finish", [](compress_utils::DecompressStream& self) {
            std::vector<uint8_t> result = self.Finish();
            return py::bytes(reinterpret_cast<const char*>(result.data()), result.size());
        },
        "Finish decompression and verify stream completeness.")
        .def("is_finished", &compress_utils::DecompressStream::IsFinished,
        "Check if the stream has been finished.")
        .def_property_readonly("algorithm", [](compress_utils::DecompressStream& self) {
            return self.algorithm();
        }, "Get the decompression algorithm being used.");

    // Functional API: Compress and Decompress
    m.def("compress", [](py::buffer data, const py::object& algorithm, int level = 3) {
        py::buffer_info info = data.request();
        const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
        size_t data_size = info.size * info.itemsize;

        std::vector<uint8_t> compressed_data = compress_utils::Compress(data_ptr, data_size, parse_algorithm(algorithm), level);

        return py::bytes(reinterpret_cast<const char*>(compressed_data.data()), compressed_data.size());
    }, py::arg("data"), py::arg("algorithm"), py::arg("level") = 3, "Compress data using an algorithm and optional level");

    m.def("decompress", [](py::buffer data, const py::object& algorithm) {
        py::buffer_info info = data.request();
        const uint8_t* data_ptr = static_cast<const uint8_t*>(info.ptr);
        size_t data_size = info.size * info.itemsize;

        std::vector<uint8_t> decompressed_data = compress_utils::Decompress(data_ptr, data_size, parse_algorithm(algorithm));

        return py::bytes(reinterpret_cast<const char*>(decompressed_data.data()), decompressed_data.size());
    }, py::arg("data"), py::arg("algorithm"), "Decompress data using an algorithm");
}
