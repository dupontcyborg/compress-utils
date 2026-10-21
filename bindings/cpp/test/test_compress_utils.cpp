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
    cu::Algorithm::Snappy, cu::Algorithm::Gzip, cu::Algorithm::Deflate,
};

static std::vector<std::uint8_t> sample(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; i++) v[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xff);
    return v;
}

/* Deterministic incompressible bytes. */
static std::vector<std::uint8_t> pseudo_random(std::size_t n, std::uint32_t seed) {
    std::vector<std::uint8_t> v(n);
    for (auto& b : v) {
        seed = seed * 1103515245u + 12345u;
        b = static_cast<std::uint8_t>(seed >> 16);
    }
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

static int test_capacity() {
    auto in = sample(32 * 1024);
    std::span<const std::uint8_t> view(in);
    for (auto a : ALL) {
        if (!cu::is_available(a)) continue;
        auto compressed = cu::compress(a, view, 5);
        std::span<const std::uint8_t> packed(compressed);
        CHECK(cu::decompress(a, packed, in.size()) == in,
              "%s exact-capacity mismatch", cu::algorithm_name(a).c_str());
        CHECK(cu::decompress(a, packed, in.size() + 4096) == in,
              "%s oversized-capacity mismatch", cu::algorithm_name(a).c_str());
        try {
            auto _ = cu::decompress(a, packed, in.size() / 2);
            CHECK(false, "%s accepted an undersized capacity", cu::algorithm_name(a).c_str());
        } catch (const cu::Error&) {
        }
    }
    return 0;
}

static int test_flush() {
    // Each flush delivers everything written so far, on one open stream.
    for (auto a : ALL) {
        if (!cu::is_available(a)) continue;
        cu::CompressStream cs(a, 5);
        if (a == cu::Algorithm::Snappy || a == cu::Algorithm::Bz2) {
            (void)cs.write(std::span<const std::uint8_t>(sample(16)));
            try {
                (void)cs.flush();
                CHECK(false, "%s flush must throw", cu::algorithm_name(a).c_str());
            } catch (const cu::Error& e) {
                CHECK(e.code() == CU_ERR_UNSUPPORTED_ALGO, "%s flush: code %d",
                      cu::algorithm_name(a).c_str(), static_cast<int>(e.code()));
            }
            continue;
        }
        cu::DecompressStream ds(a);
        std::vector<std::uint8_t> sent, received;
        // The random message doesn't compress, so flushing it takes more than
        // one 64 KiB output buffer: the binding's drain loop must run again.
        for (auto message : {sample(131072), pseudo_random(65537, 7), sample(37)}) {
            auto wire = cs.write(std::span<const std::uint8_t>(message));
            auto tail = cs.flush();
            if (a == cu::Algorithm::Zstd && message.size() == 65537) {
                CHECK(tail.size() > 64 * 1024, "zstd flush of random data: only %zu bytes",
                      tail.size());
            }
            wire.insert(wire.end(), tail.begin(), tail.end());
            auto got = ds.write(std::span<const std::uint8_t>(wire));
            sent.insert(sent.end(), message.begin(), message.end());
            received.insert(received.end(), got.begin(), got.end());
            CHECK(received == sent, "%s: %zu of %zu bytes after flush",
                  cu::algorithm_name(a).c_str(), received.size(), sent.size());
        }
        auto end = cs.finish();
        auto rest = ds.write(std::span<const std::uint8_t>(end));
        auto last = ds.finish();
        CHECK(rest.empty() && last.empty(), "%s: output after finish",
              cu::algorithm_name(a).c_str());
    }
    return 0;
}

static int test_raw_formats() {
    if (cu::is_available(cu::Algorithm::Lz4Raw)) {
        auto in = sample(32 * 1024);
        auto block = cu::compress(cu::Algorithm::Lz4Raw, std::span<const std::uint8_t>(in));
        std::span<const std::uint8_t> packed(block);
        CHECK(cu::decompress(cu::Algorithm::Lz4Raw, packed, in.size() + 100) == in,
              "lz4_raw capacity round-trip mismatch");
        CHECK(cu::decompress(cu::Algorithm::Lz4Raw, packed) == in,
              "lz4_raw incremental fallback round-trip mismatch");
        try {
            auto _ = cu::decompress(cu::Algorithm::Lz4Raw, packed, in.size() - 1);
            CHECK(false, "lz4_raw undersized capacity must throw");
        } catch (const cu::Error& e) {
            CHECK(e.code() == CU_ERR_BUF_TOO_SMALL, "lz4_raw undersized: code %d",
                  static_cast<int>(e.code()));
        }
    }
    if (cu::is_available(cu::Algorithm::Lzo)) {
        // Independent LZO1X block: 5 literals then the end marker.
        const std::uint8_t block[] = {22, 'h', 'e', 'l', 'l', 'o', 17, 0, 0};
        const std::vector<std::uint8_t> hello = {'h', 'e', 'l', 'l', 'o'};
        std::span<const std::uint8_t> packed(block);
        CHECK(cu::decompress(cu::Algorithm::Lzo, packed) == hello, "lzo size-probe decode mismatch");
        CHECK(cu::decompress(cu::Algorithm::Lzo, packed, 64) == hello, "lzo capacity decode mismatch");
        try {
            auto _ = cu::compress(cu::Algorithm::Lzo, packed);
            CHECK(false, "lzo compress must throw");
        } catch (const cu::Error& e) {
            CHECK(e.code() == CU_ERR_UNSUPPORTED_ALGO, "lzo compress: code %d",
                  static_cast<int>(e.code()));
        }
    }
    return 0;
}

/* StreamMode::Single stops at the end of the first frame: eof(), unused(),
 * and writes after the end. */
static int test_single_stream() {
    auto in = pseudo_random(20000, 7);
    const std::vector<std::uint8_t> trailer = {'Z', 'P', 'K', 'L', 1, 1, 3, 0};
    for (auto a : ALL) {
        if (!cu::is_available(a)) continue;
        const std::string algo_name = cu::algorithm_name(a);
        const char* name = algo_name.c_str();
        auto frame = cu::compress(a, std::span<const std::uint8_t>(in));
        auto wire = frame;
        wire.insert(wire.end(), trailer.begin(), trailer.end());

        cu::DecompressStream ds(a, cu::StreamMode::Single);
        CHECK(!ds.eof(), "%s: eof before any input", name);
        auto out = ds.write(std::span<const std::uint8_t>(wire));
        CHECK(out == in && ds.eof(), "%s: single-stream decode (eof %d)", name, ds.eof());
        auto unused = ds.unused();
        CHECK(std::vector<std::uint8_t>(unused.begin(), unused.end()) == trailer,
              "%s: unused is %zu bytes", name, unused.size());
        try {
            (void)ds.write(std::span<const std::uint8_t>(trailer));
            CHECK(false, "%s: write after eof must throw", name);
        } catch (const cu::Error& e) {
            CHECK(e.code() == CU_ERR_STREAM_FINISHED, "%s: write after eof: code %d", name,
                  static_cast<int>(e.code()));
        }
        CHECK(ds.write(std::span<const std::uint8_t>()).empty() && ds.finish().empty(),
              "%s: output after eof", name);
        CHECK(ds.unused().size() == trailer.size(), "%s: unused changed after eof", name);

        /* Moved-from streams report nothing. */
        cu::DecompressStream moved(std::move(ds));
        CHECK(moved.eof() && !ds.eof() && ds.unused().empty(), "%s: moved-from stream", name);

        /* The default mode reports neither. */
        cu::DecompressStream dd(a);
        auto plain = dd.write(std::span<const std::uint8_t>(frame));
        auto rest = dd.finish();
        plain.insert(plain.end(), rest.begin(), rest.end());
        CHECK(plain == in && !dd.eof() && dd.unused().empty(), "%s: default mode", name);
    }
    for (auto a : {cu::Algorithm::Lz4Raw, cu::Algorithm::Lzo}) {
        if (!cu::is_available(a)) continue;
        try {
            cu::DecompressStream ds(a, cu::StreamMode::Single);
            CHECK(false, "%s: single mode must be unsupported", cu::algorithm_name(a).c_str());
        } catch (const cu::Error& e) {
            CHECK(e.code() == CU_ERR_UNSUPPORTED_ALGO, "%s: code %d",
                  cu::algorithm_name(a).c_str(), static_cast<int>(e.code()));
        }
    }
    return 0;
}

/* Streams moved mid-stream keep going: the moved-to object owns the frame in
 * progress. And set_max_decompressed_size caps one-shot decompress. */
static int test_moves_and_cap() {
    const auto in = pseudo_random(8192, 11);
    const std::span<const std::uint8_t> head(in.data(), in.size() / 2);
    const std::span<const std::uint8_t> tail(in.data() + in.size() / 2, in.size() - head.size());

    cu::CompressStream cs(cu::Algorithm::Zstd);
    auto packed = cs.write(head);
    cu::CompressStream moved(std::move(cs));
    auto more = moved.write(tail);
    packed.insert(packed.end(), more.begin(), more.end());
    cu::CompressStream assigned(cu::Algorithm::Gzip);  /* its own stream is released */
    assigned = std::move(moved);
    more = assigned.finish();
    packed.insert(packed.end(), more.begin(), more.end());
    CHECK(cu::decompress(cu::Algorithm::Zstd, packed) == in, "moved compress stream");

    const std::span<const std::uint8_t> frame(packed);
    cu::DecompressStream ds(cu::Algorithm::Zstd);
    auto out = ds.write(frame.first(frame.size() / 2));
    cu::DecompressStream target(cu::Algorithm::Zstd);
    target = std::move(ds);
    auto rest = target.write(frame.subspan(frame.size() / 2));
    out.insert(out.end(), rest.begin(), rest.end());
    rest = target.finish();
    out.insert(out.end(), rest.begin(), rest.end());
    CHECK(out == in, "moved decompress stream");

    /* A one-shot frame declares its size, so the cap is checked up front. */
    const auto sized = cu::compress(cu::Algorithm::Zstd, in);
    cu::set_max_decompressed_size(1024);
    try {
        (void)cu::decompress(cu::Algorithm::Zstd, sized);
        cu::set_max_decompressed_size(std::size_t{1} << 30);
        CHECK(false, "decompress over the cap must throw");
    } catch (const cu::Error& e) {
        cu::set_max_decompressed_size(std::size_t{1} << 30);  /* the default */
        CHECK(e.code() == CU_ERR_SIZE_LIMIT, "over the cap: code %d", static_cast<int>(e.code()));
    }
    CHECK(cu::decompress(cu::Algorithm::Zstd, sized) == in, "decompress after the cap reset");
    return 0;
}

int main() {
    std::printf("cu version: %s\n", cu::version().c_str());
    if (test_freefn_roundtrip())  return 1;
    if (test_stream_roundtrip())  return 1;
    if (test_error_translation()) return 1;
    if (test_capacity())          return 1;
    if (test_flush())             return 1;
    if (test_raw_formats())       return 1;
    if (test_single_stream())     return 1;
    if (test_moves_and_cap())     return 1;
    std::printf("OK\n");
    return 0;
}
