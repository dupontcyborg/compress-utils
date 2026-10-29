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
 *   // Raw blocks don't record their size: pass an output capacity.
 *   auto block = cu::decompress(cu::Algorithm::Lz4Raw, raw, page_size);
 *
 *   cu::CompressStream cs(cu::Algorithm::Zstd, 5);
 *   for (auto chunk : input_chunks) {
 *       for (auto& out : cs.write(chunk)) sink.push(out);
 *   }
 *   for (auto& out : cs.finish()) sink.push(out);
 *
 *   // Long-lived stream: flush() delivers each message without ending it.
 *   sink.send(cs.write(message));
 *   sink.send(cs.flush());
 */

#ifndef COMPRESS_UTILS_HPP
#define COMPRESS_UTILS_HPP

#include <compress_utils.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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
    Snappy = CU_ALGO_SNAPPY,
    Gzip   = CU_ALGO_GZIP,
    Deflate = CU_ALGO_DEFLATE,
    Lz4Raw = CU_ALGO_LZ4_RAW,  /* raw block; incremental decompression */
    Lzo    = CU_ALGO_LZO,      /* raw LZO1X; decompression only */
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

/*
 * Decompress `in`. When the wire format records the decompressed size the output is allocated once and decoded into directly; otherwise cu_decompress_alloc grows its own buffer, and the result is copied out of it once.
 */
inline std::vector<std::uint8_t> decompress(
    Algorithm a,
    std::span<const std::uint8_t> in
) {
    std::size_t hint = 0;
    cu_status_t s = cu_decompress_size_hint(detail::c_algo(a), in.data(), in.size(), &hint);
    if (s == CU_OK) {
        std::vector<std::uint8_t> out(hint ? hint : 1);
        std::size_t out_len = hint;
        detail::check(cu_decompress(detail::c_algo(a), in.data(), in.size(),
                                    out.data(), &out_len));
        out.resize(out_len);
        return out;
    }
    if (s != CU_ERR_SIZE_UNKNOWN) detail::throw_status(s);

    std::uint8_t* grown = nullptr;
    std::size_t grown_len = 0;
    detail::check(cu_decompress_alloc(detail::c_algo(a), in.data(), in.size(), &grown, &grown_len));
    std::unique_ptr<std::uint8_t, decltype(&cu_buffer_free)> owner(grown, cu_buffer_free);
    return std::vector<std::uint8_t>(grown, grown + grown_len);
}

/*
 * Decompress into at most `capacity` bytes. The capacity is an upper bound on
 * the output, allocated up front; the result is trimmed to what was written.
 * Skips the size probe and streaming fallback for every algorithm.
 */
inline std::vector<std::uint8_t> decompress(
    Algorithm a,
    std::span<const std::uint8_t> in,
    std::size_t capacity
) {
    // A non-empty allocation keeps data() non-null for a zero capacity.
    std::vector<std::uint8_t> out(capacity ? capacity : 1);
    std::size_t out_len = capacity;
    detail::check(cu_decompress(detail::c_algo(a), in.data(), in.size(),
                                out.data(), &out_len));
    out.resize(out_len);
    return out;
}

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

    /*
     * Nonterminal flush: returns the bytes a decoder needs to recover all
     * input written so far, and keeps the frame open for more writes. Throws
     * cu::Error with CU_ERR_UNSUPPORTED_ALGO for Snappy and Bz2.
     */
    std::vector<std::uint8_t> flush() {
        return drain_loop([&](std::uint8_t* out, std::size_t* out_len, bool) {
            return cu_compress_stream_flush(stream_, out, out_len);
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

/* How a DecompressStream treats data after the end of a frame. */
enum class StreamMode {
    /* Concatenated frames/members/streams decode as one (the default). */
    Concatenated,
    /* Stop at the end of the first frame; eof() and unused() report it
     * (CU_DSTREAM_SINGLE). */
    Single,
};

class DecompressStream {
public:
    explicit DecompressStream(Algorithm a, StreamMode mode = StreamMode::Concatenated) {
        detail::check(cu_decompress_stream_create_ex(
            detail::c_algo(a), mode == StreamMode::Single ? CU_DSTREAM_SINGLE : 0u, &stream_));
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

    /* Limits the total decompressed output to `bytes` (0 = no limit, the
     * default). The write() or finish() that would go past it throws cu::Error
     * with CU_ERR_SIZE_LIMIT, and the stream is dead after that. */
    void set_max_output(std::size_t bytes) {
        detail::check(cu_decompress_stream_set_max_output(stream_, bytes));
    }

    /* StreamMode::Single: the end of the stream has been decoded and all of its
     * output returned. After that, write() with data throws cu::Error
     * (CU_ERR_STREAM_FINISHED) without consuming it; an empty write() and
     * finish() return nothing. */
    bool eof() const noexcept { return cu_decompress_stream_eof(stream_) != 0; }

    /* StreamMode::Single: the input received after the end of the stream, once
     * eof(). Valid until the next call on this stream. */
    std::span<const std::uint8_t> unused() const {
        if (!stream_) return {};
        const std::uint8_t* data = nullptr;
        std::size_t len = 0;
        detail::check(cu_decompress_stream_unused(stream_, &data, &len));
        return std::span<const std::uint8_t>(data, len);
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

}  // namespace cu

#endif  // COMPRESS_UTILS_HPP
