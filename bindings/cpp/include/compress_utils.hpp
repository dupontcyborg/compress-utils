/*
 * compress_utils.hpp — C++ binding for the compress-utils library.
 *
 * Header-only RAII wrapper over the C ABI in <compress_utils.h>. There is
 * no separate C++ shared/static library — link against `compress_utils`
 * (the C lib) and include this header.
 *
 * Example:
 *
 *   #include <compress_utils.hpp>
 *
 *   auto compressed = cu::compress(cu::Algorithm::Zstd, input, 5);
 *   auto restored   = cu::decompress(cu::Algorithm::Zstd, compressed);
 *
 *   cu::CompressStream cs(cu::Algorithm::Zstd, 5);
 *   for (auto chunk : input_chunks) {
 *       for (auto& out : cs.write(chunk)) sink.push(out);
 *   }
 *   for (auto& out : cs.finish()) sink.push(out);
 */

#ifndef COMPRESS_UTILS_HPP
#define COMPRESS_UTILS_HPP

#include <compress_utils.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cu {

/* ============================================================================
 * Types
 * ============================================================================ */

enum class Algorithm : int {
    Zstd   = CU_ALGO_ZSTD,
    Brotli = CU_ALGO_BROTLI,
    Zlib   = CU_ALGO_ZLIB,
    Bz2    = CU_ALGO_BZ2,
    Lz4    = CU_ALGO_LZ4,
    Xz     = CU_ALGO_XZ,
    Lzma   = CU_ALGO_LZMA,  /* alias for Xz */
};

class Error : public std::runtime_error {
public:
    Error(cu_status_t code, std::string msg)
        : std::runtime_error(std::move(msg)), code_(code) {}
    cu_status_t code() const noexcept { return code_; }
private:
    cu_status_t code_;
};

namespace detail {

[[noreturn]] inline void throw_status(cu_status_t s) {
    std::string msg = cu_strerror(s);
    const char* last = cu_last_error();
    if (last && *last) {
        msg.append(": ");
        msg.append(last);
    }
    throw Error(s, std::move(msg));
}

inline void check(cu_status_t s) {
    if (s != CU_OK) throw_status(s);
}

inline cu_algorithm_t c_algo(Algorithm a) {
    return static_cast<cu_algorithm_t>(a);
}

}  // namespace detail

/* ============================================================================
 * Introspection
 * ============================================================================ */

inline std::string version() {
    return cu_version();
}

inline bool is_available(Algorithm a) {
    return cu_algorithm_available(detail::c_algo(a)) != 0;
}

inline std::string algorithm_name(Algorithm a) {
    const char* n = cu_algorithm_name(detail::c_algo(a));
    return n ? std::string(n) : std::string();
}

inline void set_max_decompressed_size(std::size_t bytes) {
    cu_set_max_decompressed_size(bytes);
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

inline std::vector<std::uint8_t> compress(
    Algorithm a,
    std::span<const std::uint8_t> in,
    int level = 5
) {
    std::size_t bound = cu_compress_bound(in.size(), detail::c_algo(a));
    std::vector<std::uint8_t> out(bound);
    std::size_t out_len = bound;
    detail::check(cu_compress(detail::c_algo(a), in.data(), in.size(),
                              out.data(), &out_len, level));
    out.resize(out_len);
    return out;
}

inline std::vector<std::uint8_t> decompress(
    Algorithm a,
    std::span<const std::uint8_t> in
);  // forward decl; defined below the streaming class.

/* ============================================================================
 * Streaming
 *
 * Stream classes return std::vector<uint8_t> from write()/finish() — the
 * binding takes care of the C ABI's "fill buffer, return BUF_TOO_SMALL,
 * drain" loop internally so C++ users don't see it.
 * ============================================================================ */

class CompressStream {
public:
    CompressStream(Algorithm a, int level = 5) {
        detail::check(cu_compress_stream_create(detail::c_algo(a), level, &stream_));
    }
    CompressStream(const CompressStream&) = delete;
    CompressStream& operator=(const CompressStream&) = delete;
    CompressStream(CompressStream&& other) noexcept
        : stream_(std::exchange(other.stream_, nullptr)) {}
    CompressStream& operator=(CompressStream&& other) noexcept {
        if (this != &other) {
            destroy();
            stream_ = std::exchange(other.stream_, nullptr);
        }
        return *this;
    }
    ~CompressStream() { destroy(); }

    std::vector<std::uint8_t> write(std::span<const std::uint8_t> in) {
        return drain_loop([&](std::uint8_t* out, std::size_t* out_len, bool first) {
            return cu_compress_stream_write(
                stream_,
                first ? in.data() : nullptr,
                first ? in.size() : 0,
                out, out_len
            );
        });
    }

    std::vector<std::uint8_t> finish() {
        return drain_loop([&](std::uint8_t* out, std::size_t* out_len, bool) {
            return cu_compress_stream_finish(stream_, out, out_len);
        });
    }

private:
    cu_compress_stream_t* stream_ = nullptr;

    void destroy() {
        if (stream_) { cu_compress_stream_destroy(stream_); stream_ = nullptr; }
    }

    template <typename Op>
    std::vector<std::uint8_t> drain_loop(Op&& op) {
        std::vector<std::uint8_t> out;
        std::uint8_t scratch[64 * 1024];
        bool first = true;
        for (;;) {
            std::size_t scratch_len = sizeof(scratch);
            cu_status_t s = op(scratch, &scratch_len, first);
            first = false;
            out.insert(out.end(), scratch, scratch + scratch_len);
            if (s == CU_OK) break;
            if (s != CU_ERR_BUF_TOO_SMALL) detail::throw_status(s);
        }
        return out;
    }
};

class DecompressStream {
public:
    explicit DecompressStream(Algorithm a) {
        detail::check(cu_decompress_stream_create(detail::c_algo(a), &stream_));
    }
    DecompressStream(const DecompressStream&) = delete;
    DecompressStream& operator=(const DecompressStream&) = delete;
    DecompressStream(DecompressStream&& other) noexcept
        : stream_(std::exchange(other.stream_, nullptr)) {}
    DecompressStream& operator=(DecompressStream&& other) noexcept {
        if (this != &other) {
            destroy();
            stream_ = std::exchange(other.stream_, nullptr);
        }
        return *this;
    }
    ~DecompressStream() { destroy(); }

    std::vector<std::uint8_t> write(std::span<const std::uint8_t> in) {
        return drain_loop([&](std::uint8_t* out, std::size_t* out_len, bool first) {
            return cu_decompress_stream_write(
                stream_,
                first ? in.data() : nullptr,
                first ? in.size() : 0,
                out, out_len
            );
        });
    }

    std::vector<std::uint8_t> finish() {
        return drain_loop([&](std::uint8_t* out, std::size_t* out_len, bool) {
            return cu_decompress_stream_finish(stream_, out, out_len);
        });
    }

private:
    cu_decompress_stream_t* stream_ = nullptr;

    void destroy() {
        if (stream_) { cu_decompress_stream_destroy(stream_); stream_ = nullptr; }
    }

    template <typename Op>
    std::vector<std::uint8_t> drain_loop(Op&& op) {
        std::vector<std::uint8_t> out;
        std::uint8_t scratch[64 * 1024];
        bool first = true;
        for (;;) {
            std::size_t scratch_len = sizeof(scratch);
            cu_status_t s = op(scratch, &scratch_len, first);
            first = false;
            out.insert(out.end(), scratch, scratch + scratch_len);
            if (s == CU_OK) break;
            if (s != CU_ERR_BUF_TOO_SMALL) detail::throw_status(s);
        }
        return out;
    }
};

/* ============================================================================
 * decompress() definition — needs DecompressStream for unknown-size path.
 * ============================================================================ */

inline std::vector<std::uint8_t> decompress(
    Algorithm a,
    std::span<const std::uint8_t> in
) {
    std::size_t hint = 0;
    cu_status_t s = cu_decompress_size_hint(detail::c_algo(a), in.data(), in.size(), &hint);
    if (s == CU_OK) {
        std::vector<std::uint8_t> out(hint);
        std::size_t out_len = hint;
        detail::check(cu_decompress(detail::c_algo(a), in.data(), in.size(),
                                    out.data(), &out_len));
        out.resize(out_len);
        return out;
    }
    if (s != CU_ERR_SIZE_UNKNOWN) detail::throw_status(s);

    // Unknown size: stream-decompress.
    DecompressStream ds(a);
    auto first = ds.write(in);
    auto last  = ds.finish();
    first.insert(first.end(), last.begin(), last.end());
    return first;
}

}  // namespace cu

#endif  // COMPRESS_UTILS_HPP
