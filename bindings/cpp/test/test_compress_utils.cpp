/*
 * cpp_smoke_test.cpp — exercise the header-only C++ binding.
 *
 * Mirrors the C smoke test: round-trip every available algorithm via the
 * free functions and the streaming classes. Confirms RAII semantics by
 * letting CompressStream/DecompressStream go out of scope mid-loop.
 */

#include <compress_utils.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);   \
        std::fprintf(stderr, __VA_ARGS__);                          \
        std::fprintf(stderr, "\n");                                 \
        return 1;                                                   \
    }                                                               \
} while (0)

static constexpr cu::Algorithm ALL[] = {
    cu::Algorithm::Zstd, cu::Algorithm::Brotli, cu::Algorithm::Zlib,
    cu::Algorithm::Bz2,  cu::Algorithm::Lz4,    cu::Algorithm::Xz,
    cu::Algorithm::Snappy, cu::Algorithm::Gzip,
};

static std::vector<std::uint8_t> sample(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; i++) v[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
    return v;
}

static int test_freefn_roundtrip() {
    auto in = sample(32 * 1024);
    for (auto a : ALL) {
        if (!cu::is_available(a)) continue;
        auto compressed = cu::compress(a, std::span<const std::uint8_t>(in), 5);
        auto restored   = cu::decompress(a, std::span<const std::uint8_t>(compressed));
        CHECK(restored == in, "%s freefn round-trip mismatch", cu::algorithm_name(a).c_str());
        std::printf("  %s: %zu -> %zu\n", cu::algorithm_name(a).c_str(), in.size(), compressed.size());
    }
    return 0;
}

static int test_stream_roundtrip() {
    auto in = sample(64 * 1024);
    for (auto a : ALL) {
        if (!cu::is_available(a)) continue;

        // Compress in 3 chunks via CompressStream.
        cu::CompressStream cs(a, 5);
        std::vector<std::uint8_t> compressed;
        std::size_t chunk = in.size() / 3 + 1;
        for (std::size_t off = 0; off < in.size(); off += chunk) {
            std::size_t n = std::min(chunk, in.size() - off);
            auto piece = cs.write(std::span<const std::uint8_t>(in.data() + off, n));
            compressed.insert(compressed.end(), piece.begin(), piece.end());
        }
        auto tail = cs.finish();
        compressed.insert(compressed.end(), tail.begin(), tail.end());

        // Decompress via DecompressStream.
        cu::DecompressStream ds(a);
        auto first = ds.write(std::span<const std::uint8_t>(compressed));
        auto last  = ds.finish();
        first.insert(first.end(), last.begin(), last.end());
        CHECK(first == in, "%s stream round-trip mismatch", cu::algorithm_name(a).c_str());
    }
    return 0;
}

static int test_error_translation() {
    // Decompress garbage should throw cu::Error.
    std::vector<std::uint8_t> garbage(32, 0xff);
    try {
        auto _ = cu::decompress(cu::Algorithm::Zstd, std::span<const std::uint8_t>(garbage));
        CHECK(false, "expected cu::Error on garbage decompress");
    } catch (const cu::Error& e) {
        std::printf("  error: code=%d msg=%s\n", static_cast<int>(e.code()), e.what());
        CHECK(e.code() != CU_OK, "Error reported CU_OK");
    }
    return 0;
}

int main() {
    std::printf("cu version: %s\n", cu::version().c_str());
    if (test_freefn_roundtrip())  return 1;
    if (test_stream_roundtrip())  return 1;
    if (test_error_translation()) return 1;
    std::printf("OK\n");
    return 0;
}
