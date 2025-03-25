#include <emscripten/bind.h>
#include "compress_utils.hpp"
#include "compress_utils_func.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <stdexcept>

using namespace compress_utils;
using namespace emscripten;

// Helper function to normalize algorithm strings (trim and lowercase)
std::string normalizeAlgorithmString(const std::string& str) {
    std::string result = str;
    // Simple trim
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);
    // Convert to lowercase
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// Mapping between algorithm names and enum values
const std::unordered_map<std::string, Algorithm> algorithmMap = {
#ifdef INCLUDE_BROTLI
    {"brotli", Algorithm::BROTLI},
#endif
#ifdef INCLUDE_XZ
    {"xz", Algorithm::XZ},
    {"lzma", Algorithm::LZMA},
#endif
#ifdef INCLUDE_ZLIB
    {"zlib", Algorithm::ZLIB},
#endif
#ifdef INCLUDE_ZSTD
    {"zstd", Algorithm::ZSTD},
#endif
};

// Convert string to Algorithm enum
Algorithm stringToAlgorithm(const std::string& algorithmStr) {
    std::string normalized = normalizeAlgorithmString(algorithmStr);
    auto it = algorithmMap.find(normalized);
    if (it != algorithmMap.end()) {
        return it->second;
    }
    throw std::invalid_argument("Unknown algorithm: " + algorithmStr);
}

// Get available algorithms as array
val getAvailableAlgorithms() {
    val result = val::array();
    unsigned index = 0;
    
    for (const auto& entry : algorithmMap) {
        result.set(index++, entry.first);
    }
    
    return result;
}

// Convert JS ArrayBuffer/TypedArray/Array to a vector of bytes efficiently
std::vector<uint8_t> getBytesFromJSValue(val data) {
    // Handle ArrayBuffer
    if (data.instanceof(val::global("ArrayBuffer"))) {
        data = val::global("Uint8Array").new_(data);
    }
    
    unsigned length = data["length"].as<unsigned>();
    std::vector<uint8_t> result(length);
    
    // Handle TypedArray with optimized chunk copying for better performance
    if (data.instanceof(val::global("Uint8Array")) || 
        data.instanceof(val::global("Int8Array"))) {
        const unsigned CHUNK_SIZE = 4096;
        
        for (unsigned offset = 0; offset < length; offset += CHUNK_SIZE) {
            unsigned chunk_size = std::min(CHUNK_SIZE, length - offset);
            
            // Copy chunk by chunk to avoid excessive JS->C++ calls
            for (unsigned i = 0; i < chunk_size; ++i) {
                result[offset + i] = data[offset + i].as<uint8_t>();
            }
        }
        return result;
    }
    
    // Handle regular Array (less efficient but still chunked)
    const unsigned CHUNK_SIZE = 4096;
    for (unsigned offset = 0; offset < length; offset += CHUNK_SIZE) {
        unsigned chunk_size = std::min(CHUNK_SIZE, length - offset);
        for (unsigned i = 0; i < chunk_size; ++i) {
            result[offset + i] = data[offset + i].as<uint8_t>();
        }
    }
    return result;
}

// Convert a vector of bytes to a Uint8Array efficiently
val createUint8Array(const std::vector<uint8_t>& data) {
    const size_t size = data.size();
    val result = val::global("Uint8Array").new_(size);
    
    // Use chunked copying for better performance
    const size_t CHUNK_SIZE = 4096;
    for (size_t offset = 0; offset < size; offset += CHUNK_SIZE) {
        size_t chunk_size = std::min<size_t>(CHUNK_SIZE, size - offset);
        for (size_t i = 0; i < chunk_size; ++i) {
            result.set(offset + i, data[offset + i]);
        }
    }
    return result;
}

// Create JavaScript Error object
val createError(const std::string& name, const std::string& message, const std::string& algorithm = "") {
    val Error = val::global("Error");
    val error = Error.new_(val(message));
    error.set("name", val(name));
    if (!algorithm.empty()) {
        error.set("algorithm", val(algorithm));
    }
    return error;
}

// Compressor class implementation with error handling
class CompressorWrapper {
private:
    std::unique_ptr<Compressor> compressor;
    std::string algorithm_name;

public:
    CompressorWrapper(const std::string& algorithm) {
        try {
            algorithm_name = normalizeAlgorithmString(algorithm);
            Algorithm alg = stringToAlgorithm(algorithm_name);
            compressor = std::make_unique<Compressor>(alg);
        } catch (const std::exception& e) {
            throw createError("InvalidArgumentError", e.what());
        }
    }
    
    val compress(val data, int level = 3) {
        try {
            if (level < 0 || level > 9) {
                throw std::invalid_argument("Compression level must be between 0 and 9");
            }
            
            std::vector<uint8_t> input = getBytesFromJSValue(data);
            std::vector<uint8_t> result = compressor->Compress(input.data(), input.size(), level);
            return createUint8Array(result);
        } catch (const std::invalid_argument& e) {
            throw createError("InvalidArgumentError", e.what());
        } catch (const std::exception& e) {
            throw createError("CompressionError", 
                              std::string("Compression error: ") + e.what(), 
                              algorithm_name);
        } catch (...) {
            throw createError("CompressionError", 
                              "Unknown error occurred during compression", 
                              algorithm_name);
        }
    }
    
    val decompress(val data) {
        try {
            std::vector<uint8_t> input = getBytesFromJSValue(data);
            
            if (input.empty()) {
                throw std::invalid_argument("Cannot decompress empty data");
            }
            
            std::vector<uint8_t> result = compressor->Decompress(input.data(), input.size());
            return createUint8Array(result);
        } catch (const std::invalid_argument& e) {
            throw createError("InvalidArgumentError", e.what());
        } catch (const std::exception& e) {
            throw createError("DecompressionError", 
                              std::string("Decompression error: ") + e.what(), 
                              algorithm_name);
        } catch (...) {
            throw createError("DecompressionError", 
                              "Unknown error occurred during decompression", 
                              algorithm_name);
        }
    }
};

// Functional API implementations with error handling
val compressFunc(val data, const std::string& algorithm, int level = 3) {
    std::string algorithm_name;
    
    try {
        algorithm_name = normalizeAlgorithmString(algorithm);
        
        if (level < 0 || level > 9) {
            throw std::invalid_argument("Compression level must be between 0 and 9");
        }
        
        Algorithm alg = stringToAlgorithm(algorithm_name);
        std::vector<uint8_t> input = getBytesFromJSValue(data);
        std::vector<uint8_t> result = Compress(input.data(), input.size(), alg, level);
        return createUint8Array(result);
    } catch (const std::invalid_argument& e) {
        throw createError("InvalidArgumentError", e.what());
    } catch (const std::exception& e) {
        throw createError("CompressionError", 
                          std::string("Compression error: ") + e.what(), 
                          algorithm_name);
    } catch (...) {
        throw createError("CompressionError", 
                          "Unknown error occurred during compression", 
                          algorithm_name);
    }
}

val decompressFunc(val data, const std::string& algorithm) {
    std::string algorithm_name;
    
    try {
        algorithm_name = normalizeAlgorithmString(algorithm);
        Algorithm alg = stringToAlgorithm(algorithm_name);
        
        std::vector<uint8_t> input = getBytesFromJSValue(data);
        
        if (input.empty()) {
            throw std::invalid_argument("Cannot decompress empty data");
        }
        
        std::vector<uint8_t> result = Decompress(input.data(), input.size(), alg);
        return createUint8Array(result);
    } catch (const std::invalid_argument& e) {
        throw createError("InvalidArgumentError", e.what());
    } catch (const std::exception& e) {
        throw createError("DecompressionError", 
                          std::string("Decompression error: ") + e.what(), 
                          algorithm_name);
    } catch (...) {
        throw createError("DecompressionError", 
                          "Unknown error occurred during decompression", 
                          algorithm_name);
    }
}

// Bindings for the module
EMSCRIPTEN_BINDINGS(compress_utils_module) {
    // OOP API: Compressor class
    class_<CompressorWrapper>("Compressor")
        .constructor<const std::string&>()
        .function("compress", &CompressorWrapper::compress)
        .function("decompress", &CompressorWrapper::decompress);
    
    // Functional API
    function("compress", &compressFunc);
    function("decompress", &decompressFunc);
    
    // Algorithm information functions
    function("getAvailableAlgorithms", &getAvailableAlgorithms);
}