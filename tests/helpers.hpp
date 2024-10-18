#ifndef TESTS_HELPERS_HPP_
#define TESTS_HELPERS_HPP_

#include "algorithms.hpp"

#include <gtest/gtest.h>
#include <random>

// Helper function to generate random data
inline std::vector<uint8_t> GenerateData(size_t size_in_bytes) {
    std::vector<uint8_t> data(size_in_bytes);
    std::mt19937 rng(42);  // Use a fixed seed for reproducibility
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < size_in_bytes; ++i) {
        data[i] = static_cast<uint8_t>(dist(rng));
    }
    return data;
}

// Helper function to generate repetitive data
inline std::vector<uint8_t> GenerateRepetitiveData(size_t size_in_bytes, uint8_t value) {
    return std::vector<uint8_t>(size_in_bytes, value);
}

// Helper function to generate test names based on the Algorithm enum
inline std::string AlgorithmToString(const ::testing::TestParamInfo<compression_utils::Algorithm>& info) {
    switch (info.param) {
#ifdef INCLUDE_ZLIB
        case compression_utils::Algorithm::ZLIB:
            return "ZLIB";
#endif
#ifdef INCLUDE_ZSTD
        case compression_utils::Algorithm::ZSTD:
            return "ZSTD";
#endif
        default:
            return "UnknownAlgorithm";
    }
}

#endif  // TESTS_HELPERS_HPP_