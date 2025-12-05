#include "compress_utils_stream.hpp"
#include "utils/constants.hpp"

#include <stdexcept>
#include <string>

// Include algorithm headers conditionally
#ifdef INCLUDE_ZSTD
#include "zstd/zstd.h"
#endif

#ifdef INCLUDE_BROTLI
#include "brotli/decode.h"
#include "brotli/encode.h"
#endif

#ifdef INCLUDE_BZ2
#include "bz2/bzlib.h"
#endif

#ifdef INCLUDE_LZ4
#include "lz4/lz4.h"
#include "lz4/lz4hc.h"
#include "lz4/lz4frame.h"
#endif

#ifdef INCLUDE_ZLIB
#include "zlib/zlib.h"
#endif

#ifdef INCLUDE_XZ
#include "xz/lzma.h"
#endif

namespace compress_utils {

namespace {

// Default buffer size for streaming operations
constexpr size_t kDefaultBufferSize = 64 * 1024;  // 64 KB

}  // namespace

// ============================================================================
// CompressStream Implementation
// ============================================================================

struct CompressStream::Impl {
    Algorithm algorithm;
    int level;
    bool finished = false;
    std::vector<uint8_t> output_buffer;

    // Algorithm-specific state (only one will be used)
#ifdef INCLUDE_ZSTD
    ZSTD_CStream* zstd_stream = nullptr;
#endif
#ifdef INCLUDE_BROTLI
    BrotliEncoderState* brotli_state = nullptr;
#endif
#ifdef INCLUDE_BZ2
    bz_stream bz2_stream = {};
    bool bz2_initialized = false;
#endif
#ifdef INCLUDE_LZ4
    LZ4F_cctx* lz4_cctx = nullptr;
    bool lz4_header_written = false;
#endif
#ifdef INCLUDE_ZLIB
    z_stream zlib_stream = {};
    bool zlib_initialized = false;
#endif
#ifdef INCLUDE_XZ
    lzma_stream xz_stream = LZMA_STREAM_INIT;
    bool xz_initialized = false;
#endif

    Impl(Algorithm algo, int lvl) : algorithm(algo), level(lvl) {
        output_buffer.resize(kDefaultBufferSize);
    }

    ~Impl() {
        Cleanup();
    }

    // Non-copyable and non-movable - the unique_ptr handles ownership
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void Cleanup() {
#ifdef INCLUDE_ZSTD
        if (zstd_stream) {
            ZSTD_freeCStream(zstd_stream);
            zstd_stream = nullptr;
        }
#endif
#ifdef INCLUDE_BROTLI
        if (brotli_state) {
            BrotliEncoderDestroyInstance(brotli_state);
            brotli_state = nullptr;
        }
#endif
#ifdef INCLUDE_BZ2
        if (bz2_initialized) {
            BZ2_bzCompressEnd(&bz2_stream);
            bz2_initialized = false;
        }
#endif
#ifdef INCLUDE_LZ4
        if (lz4_cctx) {
            LZ4F_freeCompressionContext(lz4_cctx);
            lz4_cctx = nullptr;
        }
#endif
#ifdef INCLUDE_ZLIB
        if (zlib_initialized) {
            deflateEnd(&zlib_stream);
            zlib_initialized = false;
        }
#endif
#ifdef INCLUDE_XZ
        if (xz_initialized) {
            lzma_end(&xz_stream);
            xz_initialized = false;
        }
#endif
    }
};

CompressStream::CompressStream(Algorithm algorithm, int level)
    : impl_(std::make_unique<Impl>(algorithm, level)) {
    internal::ValidateLevel(level);

    switch (algorithm) {
#ifdef INCLUDE_ZSTD
        case Algorithm::ZSTD: {
            impl_->zstd_stream = ZSTD_createCStream();
            if (!impl_->zstd_stream) {
                throw std::runtime_error("Failed to create ZSTD compression stream");
            }
            // Map level 1-10 to ZSTD 1-22
            int zstd_level = (level * 22) / 10;
            size_t result = ZSTD_initCStream(impl_->zstd_stream, zstd_level);
            if (ZSTD_isError(result)) {
                throw std::runtime_error("Failed to initialize ZSTD compression stream: " +
                                         std::string(ZSTD_getErrorName(result)));
            }
            break;
        }
#endif
#ifdef INCLUDE_BROTLI
        case Algorithm::BROTLI: {
            impl_->brotli_state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
            if (!impl_->brotli_state) {
                throw std::runtime_error("Failed to create Brotli compression stream");
            }
            // Map level 1-10 to Brotli 0-11
            int brotli_level = (level * 11) / 10;
            BrotliEncoderSetParameter(impl_->brotli_state, BROTLI_PARAM_QUALITY, brotli_level);
            break;
        }
#endif
#ifdef INCLUDE_BZ2
        case Algorithm::BZ2: {
            impl_->bz2_stream.bzalloc = nullptr;
            impl_->bz2_stream.bzfree = nullptr;
            impl_->bz2_stream.opaque = nullptr;
            // Map level 1-10 to bzip2 1-9
            int bz2_level = (level * 9) / 10;
            if (bz2_level < 1) bz2_level = 1;
            int result = BZ2_bzCompressInit(&impl_->bz2_stream, bz2_level, 0, 0);
            if (result != BZ_OK) {
                throw std::runtime_error("Failed to initialize bzip2 compression stream");
            }
            impl_->bz2_initialized = true;
            break;
        }
#endif
#ifdef INCLUDE_LZ4
        case Algorithm::LZ4: {
            LZ4F_errorCode_t err = LZ4F_createCompressionContext(&impl_->lz4_cctx, LZ4F_VERSION);
            if (LZ4F_isError(err)) {
                throw std::runtime_error("Failed to create LZ4 compression context: " +
                                         std::string(LZ4F_getErrorName(err)));
            }
            impl_->lz4_header_written = false;
            break;
        }
#endif
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB: {
            impl_->zlib_stream.zalloc = Z_NULL;
            impl_->zlib_stream.zfree = Z_NULL;
            impl_->zlib_stream.opaque = Z_NULL;
            // Map level 1-10 to zlib 1-9
            int zlib_level = (level > 9) ? 9 : level;
            int result = deflateInit(&impl_->zlib_stream, zlib_level);
            if (result != Z_OK) {
                throw std::runtime_error("Failed to initialize zlib compression stream");
            }
            impl_->zlib_initialized = true;
            break;
        }
#endif
#ifdef INCLUDE_XZ
        case Algorithm::XZ:
        case Algorithm::LZMA: {
            // Map level 1-10 to XZ 0-9
            uint32_t xz_level = static_cast<uint32_t>(level - 1);
            lzma_ret result = lzma_easy_encoder(&impl_->xz_stream, xz_level, LZMA_CHECK_CRC64);
            if (result != LZMA_OK) {
                throw std::runtime_error("Failed to initialize XZ compression stream");
            }
            impl_->xz_initialized = true;
            break;
        }
#endif
        default:
            throw std::invalid_argument("Unsupported compression algorithm");
    }
}

CompressStream::~CompressStream() = default;

CompressStream::CompressStream(CompressStream&& other) noexcept = default;
CompressStream& CompressStream::operator=(CompressStream&& other) noexcept = default;

std::vector<uint8_t> CompressStream::Compress(std::span<const uint8_t> data) {
    if (impl_->finished) {
        throw std::runtime_error("Cannot compress: stream is already finished");
    }

    std::vector<uint8_t> result;

    switch (impl_->algorithm) {
#ifdef INCLUDE_ZSTD
        case Algorithm::ZSTD: {
            ZSTD_inBuffer input = {data.data(), data.size(), 0};
            while (input.pos < input.size) {
                ZSTD_outBuffer output = {impl_->output_buffer.data(), impl_->output_buffer.size(),
                                         0};
                size_t ret = ZSTD_compressStream(impl_->zstd_stream, &output, &input);
                if (ZSTD_isError(ret)) {
                    throw std::runtime_error("ZSTD compression error: " +
                                             std::string(ZSTD_getErrorName(ret)));
                }
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + output.pos);
            }
            break;
        }
#endif
#ifdef INCLUDE_BROTLI
        case Algorithm::BROTLI: {
            const uint8_t* next_in = data.data();
            size_t available_in = data.size();

            while (available_in > 0 || BrotliEncoderHasMoreOutput(impl_->brotli_state)) {
                uint8_t* next_out = impl_->output_buffer.data();
                size_t available_out = impl_->output_buffer.size();

                if (!BrotliEncoderCompressStream(impl_->brotli_state, BROTLI_OPERATION_PROCESS,
                                                 &available_in, &next_in, &available_out, &next_out,
                                                 nullptr)) {
                    throw std::runtime_error("Brotli compression error");
                }

                size_t bytes_written = impl_->output_buffer.size() - available_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);

                if (available_in == 0 && !BrotliEncoderHasMoreOutput(impl_->brotli_state)) {
                    break;
                }
            }
            break;
        }
#endif
#ifdef INCLUDE_BZ2
        case Algorithm::BZ2: {
            impl_->bz2_stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(data.data()));
            impl_->bz2_stream.avail_in = static_cast<unsigned int>(data.size());

            while (impl_->bz2_stream.avail_in > 0) {
                impl_->bz2_stream.next_out = reinterpret_cast<char*>(impl_->output_buffer.data());
                impl_->bz2_stream.avail_out = static_cast<unsigned int>(impl_->output_buffer.size());

                int ret = BZ2_bzCompress(&impl_->bz2_stream, BZ_RUN);
                if (ret != BZ_RUN_OK) {
                    throw std::runtime_error("bzip2 compression error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->bz2_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);
            }
            break;
        }
#endif
#ifdef INCLUDE_LZ4
        case Algorithm::LZ4: {
            // Write header on first compress call
            if (!impl_->lz4_header_written) {
                LZ4F_preferences_t prefs = {};
                // Map level 1-10: levels 1-3 use fast mode, 4-10 use HC mode
                if (impl_->level <= 3) {
                    prefs.compressionLevel = 0;  // Default fast
                } else {
                    // Map 4-10 to LZ4HC levels 1-12
                    prefs.compressionLevel = ((impl_->level - 3) * 12) / 7;
                    if (prefs.compressionLevel < 1) prefs.compressionLevel = 1;
                }

                size_t header_size = LZ4F_compressBegin(impl_->lz4_cctx,
                                                         impl_->output_buffer.data(),
                                                         impl_->output_buffer.size(),
                                                         &prefs);
                if (LZ4F_isError(header_size)) {
                    throw std::runtime_error("LZ4 compression begin error: " +
                                             std::string(LZ4F_getErrorName(header_size)));
                }
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + header_size);
                impl_->lz4_header_written = true;
            }

            // Compress the data
            if (data.size() > 0) {
                // Calculate required output buffer size using LZ4F_compressBound
                size_t bound = LZ4F_compressBound(data.size(), nullptr);
                // Ensure output buffer is large enough
                std::vector<uint8_t> compress_buffer(bound);

                size_t compressed_size = LZ4F_compressUpdate(impl_->lz4_cctx,
                                                              compress_buffer.data(),
                                                              compress_buffer.size(),
                                                              data.data(),
                                                              data.size(),
                                                              nullptr);
                if (LZ4F_isError(compressed_size)) {
                    throw std::runtime_error("LZ4 compression error: " +
                                             std::string(LZ4F_getErrorName(compressed_size)));
                }
                result.insert(result.end(), compress_buffer.data(),
                              compress_buffer.data() + compressed_size);
            }
            break;
        }
#endif
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB: {
            impl_->zlib_stream.next_in = const_cast<Bytef*>(data.data());
            impl_->zlib_stream.avail_in = static_cast<uInt>(data.size());

            while (impl_->zlib_stream.avail_in > 0) {
                impl_->zlib_stream.next_out = impl_->output_buffer.data();
                impl_->zlib_stream.avail_out = static_cast<uInt>(impl_->output_buffer.size());

                int ret = deflate(&impl_->zlib_stream, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR) {
                    throw std::runtime_error("zlib compression error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->zlib_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);
            }
            break;
        }
#endif
#ifdef INCLUDE_XZ
        case Algorithm::XZ:
        case Algorithm::LZMA: {
            impl_->xz_stream.next_in = data.data();
            impl_->xz_stream.avail_in = data.size();

            while (impl_->xz_stream.avail_in > 0) {
                impl_->xz_stream.next_out = impl_->output_buffer.data();
                impl_->xz_stream.avail_out = impl_->output_buffer.size();

                lzma_ret ret = lzma_code(&impl_->xz_stream, LZMA_RUN);
                if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
                    throw std::runtime_error("XZ compression error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->xz_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);
            }
            break;
        }
#endif
        default:
            throw std::invalid_argument("Unsupported compression algorithm");
    }

    return result;
}

std::vector<uint8_t> CompressStream::Finish() {
    if (impl_->finished) {
        throw std::runtime_error("Cannot finish: stream is already finished");
    }

    std::vector<uint8_t> result;
    impl_->finished = true;

    switch (impl_->algorithm) {
#ifdef INCLUDE_ZSTD
        case Algorithm::ZSTD: {
            ZSTD_inBuffer input = {nullptr, 0, 0};
            size_t remaining;
            do {
                ZSTD_outBuffer output = {impl_->output_buffer.data(), impl_->output_buffer.size(),
                                         0};
                remaining = ZSTD_endStream(impl_->zstd_stream, &output);
                if (ZSTD_isError(remaining)) {
                    throw std::runtime_error("ZSTD finish error: " +
                                             std::string(ZSTD_getErrorName(remaining)));
                }
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + output.pos);
            } while (remaining > 0);
            break;
        }
#endif
#ifdef INCLUDE_BROTLI
        case Algorithm::BROTLI: {
            const uint8_t* next_in = nullptr;
            size_t available_in = 0;

            while (!BrotliEncoderIsFinished(impl_->brotli_state)) {
                uint8_t* next_out = impl_->output_buffer.data();
                size_t available_out = impl_->output_buffer.size();

                if (!BrotliEncoderCompressStream(impl_->brotli_state, BROTLI_OPERATION_FINISH,
                                                 &available_in, &next_in, &available_out, &next_out,
                                                 nullptr)) {
                    throw std::runtime_error("Brotli finish error");
                }

                size_t bytes_written = impl_->output_buffer.size() - available_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);
            }
            break;
        }
#endif
#ifdef INCLUDE_BZ2
        case Algorithm::BZ2: {
            int ret;
            do {
                impl_->bz2_stream.next_out = reinterpret_cast<char*>(impl_->output_buffer.data());
                impl_->bz2_stream.avail_out = static_cast<unsigned int>(impl_->output_buffer.size());

                ret = BZ2_bzCompress(&impl_->bz2_stream, BZ_FINISH);
                if (ret != BZ_FINISH_OK && ret != BZ_STREAM_END) {
                    throw std::runtime_error("bzip2 finish error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->bz2_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);
            } while (ret != BZ_STREAM_END);
            break;
        }
#endif
#ifdef INCLUDE_LZ4
        case Algorithm::LZ4: {
            // If no data was ever compressed, write the header first
            if (!impl_->lz4_header_written) {
                LZ4F_preferences_t prefs = {};
                size_t header_size = LZ4F_compressBegin(impl_->lz4_cctx,
                                                         impl_->output_buffer.data(),
                                                         impl_->output_buffer.size(),
                                                         &prefs);
                if (LZ4F_isError(header_size)) {
                    throw std::runtime_error("LZ4 compression begin error: " +
                                             std::string(LZ4F_getErrorName(header_size)));
                }
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + header_size);
            }

            // Write the end frame
            size_t end_size = LZ4F_compressEnd(impl_->lz4_cctx,
                                                impl_->output_buffer.data(),
                                                impl_->output_buffer.size(),
                                                nullptr);
            if (LZ4F_isError(end_size)) {
                throw std::runtime_error("LZ4 compression end error: " +
                                         std::string(LZ4F_getErrorName(end_size)));
            }
            result.insert(result.end(), impl_->output_buffer.data(),
                          impl_->output_buffer.data() + end_size);
            break;
        }
#endif
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB: {
            impl_->zlib_stream.next_in = nullptr;
            impl_->zlib_stream.avail_in = 0;

            int ret;
            do {
                impl_->zlib_stream.next_out = impl_->output_buffer.data();
                impl_->zlib_stream.avail_out = static_cast<uInt>(impl_->output_buffer.size());

                ret = deflate(&impl_->zlib_stream, Z_FINISH);
                if (ret == Z_STREAM_ERROR) {
                    throw std::runtime_error("zlib finish error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->zlib_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);
            } while (ret != Z_STREAM_END);
            break;
        }
#endif
#ifdef INCLUDE_XZ
        case Algorithm::XZ:
        case Algorithm::LZMA: {
            impl_->xz_stream.next_in = nullptr;
            impl_->xz_stream.avail_in = 0;

            lzma_ret ret;
            do {
                impl_->xz_stream.next_out = impl_->output_buffer.data();
                impl_->xz_stream.avail_out = impl_->output_buffer.size();

                ret = lzma_code(&impl_->xz_stream, LZMA_FINISH);
                if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
                    throw std::runtime_error("XZ finish error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->xz_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);
            } while (ret != LZMA_STREAM_END);
            break;
        }
#endif
        default:
            throw std::invalid_argument("Unsupported compression algorithm");
    }

    return result;
}

bool CompressStream::IsFinished() const {
    return impl_->finished;
}

Algorithm CompressStream::algorithm() const {
    return impl_->algorithm;
}

// ============================================================================
// DecompressStream Implementation
// ============================================================================

struct DecompressStream::Impl {
    Algorithm algorithm;
    bool finished = false;
    std::vector<uint8_t> output_buffer;

    // Algorithm-specific state
#ifdef INCLUDE_ZSTD
    ZSTD_DStream* zstd_stream = nullptr;
#endif
#ifdef INCLUDE_BROTLI
    BrotliDecoderState* brotli_state = nullptr;
#endif
#ifdef INCLUDE_BZ2
    bz_stream bz2_stream = {};
    bool bz2_initialized = false;
#endif
#ifdef INCLUDE_LZ4
    LZ4F_dctx* lz4_dctx = nullptr;
#endif
#ifdef INCLUDE_ZLIB
    z_stream zlib_stream = {};
    bool zlib_initialized = false;
#endif
#ifdef INCLUDE_XZ
    lzma_stream xz_stream = LZMA_STREAM_INIT;
    bool xz_initialized = false;
#endif

    explicit Impl(Algorithm algo) : algorithm(algo) {
        output_buffer.resize(kDefaultBufferSize);
    }

    ~Impl() {
        Cleanup();
    }

    // Non-copyable and non-movable - the unique_ptr handles ownership
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void Cleanup() {
#ifdef INCLUDE_ZSTD
        if (zstd_stream) {
            ZSTD_freeDStream(zstd_stream);
            zstd_stream = nullptr;
        }
#endif
#ifdef INCLUDE_BROTLI
        if (brotli_state) {
            BrotliDecoderDestroyInstance(brotli_state);
            brotli_state = nullptr;
        }
#endif
#ifdef INCLUDE_BZ2
        if (bz2_initialized) {
            BZ2_bzDecompressEnd(&bz2_stream);
            bz2_initialized = false;
        }
#endif
#ifdef INCLUDE_LZ4
        if (lz4_dctx) {
            LZ4F_freeDecompressionContext(lz4_dctx);
            lz4_dctx = nullptr;
        }
#endif
#ifdef INCLUDE_ZLIB
        if (zlib_initialized) {
            inflateEnd(&zlib_stream);
            zlib_initialized = false;
        }
#endif
#ifdef INCLUDE_XZ
        if (xz_initialized) {
            lzma_end(&xz_stream);
            xz_initialized = false;
        }
#endif
    }
};

DecompressStream::DecompressStream(Algorithm algorithm) : impl_(std::make_unique<Impl>(algorithm)) {
    switch (algorithm) {
#ifdef INCLUDE_ZSTD
        case Algorithm::ZSTD: {
            impl_->zstd_stream = ZSTD_createDStream();
            if (!impl_->zstd_stream) {
                throw std::runtime_error("Failed to create ZSTD decompression stream");
            }
            size_t result = ZSTD_initDStream(impl_->zstd_stream);
            if (ZSTD_isError(result)) {
                throw std::runtime_error("Failed to initialize ZSTD decompression stream: " +
                                         std::string(ZSTD_getErrorName(result)));
            }
            break;
        }
#endif
#ifdef INCLUDE_BROTLI
        case Algorithm::BROTLI: {
            impl_->brotli_state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
            if (!impl_->brotli_state) {
                throw std::runtime_error("Failed to create Brotli decompression stream");
            }
            break;
        }
#endif
#ifdef INCLUDE_BZ2
        case Algorithm::BZ2: {
            impl_->bz2_stream.bzalloc = nullptr;
            impl_->bz2_stream.bzfree = nullptr;
            impl_->bz2_stream.opaque = nullptr;
            int result = BZ2_bzDecompressInit(&impl_->bz2_stream, 0, 0);
            if (result != BZ_OK) {
                throw std::runtime_error("Failed to initialize bzip2 decompression stream");
            }
            impl_->bz2_initialized = true;
            break;
        }
#endif
#ifdef INCLUDE_LZ4
        case Algorithm::LZ4: {
            LZ4F_errorCode_t err = LZ4F_createDecompressionContext(&impl_->lz4_dctx, LZ4F_VERSION);
            if (LZ4F_isError(err)) {
                throw std::runtime_error("Failed to create LZ4 decompression context: " +
                                         std::string(LZ4F_getErrorName(err)));
            }
            break;
        }
#endif
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB: {
            impl_->zlib_stream.zalloc = Z_NULL;
            impl_->zlib_stream.zfree = Z_NULL;
            impl_->zlib_stream.opaque = Z_NULL;
            impl_->zlib_stream.next_in = Z_NULL;
            impl_->zlib_stream.avail_in = 0;
            int result = inflateInit(&impl_->zlib_stream);
            if (result != Z_OK) {
                throw std::runtime_error("Failed to initialize zlib decompression stream");
            }
            impl_->zlib_initialized = true;
            break;
        }
#endif
#ifdef INCLUDE_XZ
        case Algorithm::XZ:
        case Algorithm::LZMA: {
            lzma_ret result =
                lzma_auto_decoder(&impl_->xz_stream, UINT64_MAX, LZMA_CONCATENATED);
            if (result != LZMA_OK) {
                throw std::runtime_error("Failed to initialize XZ decompression stream");
            }
            impl_->xz_initialized = true;
            break;
        }
#endif
        default:
            throw std::invalid_argument("Unsupported decompression algorithm");
    }
}

DecompressStream::~DecompressStream() = default;

DecompressStream::DecompressStream(DecompressStream&& other) noexcept = default;
DecompressStream& DecompressStream::operator=(DecompressStream&& other) noexcept = default;

std::vector<uint8_t> DecompressStream::Decompress(std::span<const uint8_t> data) {
    if (impl_->finished) {
        throw std::runtime_error("Cannot decompress: stream is already finished");
    }

    std::vector<uint8_t> result;

    switch (impl_->algorithm) {
#ifdef INCLUDE_ZSTD
        case Algorithm::ZSTD: {
            ZSTD_inBuffer input = {data.data(), data.size(), 0};
            while (input.pos < input.size) {
                ZSTD_outBuffer output = {impl_->output_buffer.data(), impl_->output_buffer.size(),
                                         0};
                size_t ret = ZSTD_decompressStream(impl_->zstd_stream, &output, &input);
                if (ZSTD_isError(ret)) {
                    throw std::runtime_error("ZSTD decompression error: " +
                                             std::string(ZSTD_getErrorName(ret)));
                }
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + output.pos);
            }
            break;
        }
#endif
#ifdef INCLUDE_BROTLI
        case Algorithm::BROTLI: {
            const uint8_t* next_in = data.data();
            size_t available_in = data.size();

            while (available_in > 0 || BrotliDecoderHasMoreOutput(impl_->brotli_state)) {
                uint8_t* next_out = impl_->output_buffer.data();
                size_t available_out = impl_->output_buffer.size();

                BrotliDecoderResult brotli_result = BrotliDecoderDecompressStream(
                    impl_->brotli_state, &available_in, &next_in, &available_out, &next_out,
                    nullptr);

                if (brotli_result == BROTLI_DECODER_RESULT_ERROR) {
                    BrotliDecoderErrorCode error = BrotliDecoderGetErrorCode(impl_->brotli_state);
                    throw std::runtime_error("Brotli decompression error: " +
                                             std::string(BrotliDecoderErrorString(error)));
                }

                size_t bytes_written = impl_->output_buffer.size() - available_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);

                if (brotli_result == BROTLI_DECODER_RESULT_SUCCESS) {
                    break;
                }
                if (available_in == 0 && !BrotliDecoderHasMoreOutput(impl_->brotli_state)) {
                    break;
                }
            }
            break;
        }
#endif
#ifdef INCLUDE_BZ2
        case Algorithm::BZ2: {
            impl_->bz2_stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(data.data()));
            impl_->bz2_stream.avail_in = static_cast<unsigned int>(data.size());

            while (impl_->bz2_stream.avail_in > 0) {
                impl_->bz2_stream.next_out = reinterpret_cast<char*>(impl_->output_buffer.data());
                impl_->bz2_stream.avail_out = static_cast<unsigned int>(impl_->output_buffer.size());

                int ret = BZ2_bzDecompress(&impl_->bz2_stream);
                if (ret != BZ_OK && ret != BZ_STREAM_END) {
                    throw std::runtime_error("bzip2 decompression error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->bz2_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);

                if (ret == BZ_STREAM_END) {
                    break;
                }
            }
            break;
        }
#endif
#ifdef INCLUDE_LZ4
        case Algorithm::LZ4: {
            const uint8_t* src = data.data();
            size_t src_size = data.size();

            while (src_size > 0) {
                size_t dst_size = impl_->output_buffer.size();
                size_t consumed = src_size;

                size_t ret = LZ4F_decompress(impl_->lz4_dctx,
                                              impl_->output_buffer.data(), &dst_size,
                                              src, &consumed,
                                              nullptr);
                if (LZ4F_isError(ret)) {
                    throw std::runtime_error("LZ4 decompression error: " +
                                             std::string(LZ4F_getErrorName(ret)));
                }

                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + dst_size);

                src += consumed;
                src_size -= consumed;

                // If ret is 0, we've finished decompressing
                if (ret == 0) {
                    break;
                }
            }
            break;
        }
#endif
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB: {
            impl_->zlib_stream.next_in = const_cast<Bytef*>(data.data());
            impl_->zlib_stream.avail_in = static_cast<uInt>(data.size());

            while (impl_->zlib_stream.avail_in > 0) {
                impl_->zlib_stream.next_out = impl_->output_buffer.data();
                impl_->zlib_stream.avail_out = static_cast<uInt>(impl_->output_buffer.size());

                int ret = inflate(&impl_->zlib_stream, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                    throw std::runtime_error("zlib decompression error: code " + std::to_string(ret));
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->zlib_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);

                if (ret == Z_STREAM_END) {
                    break;
                }
            }
            break;
        }
#endif
#ifdef INCLUDE_XZ
        case Algorithm::XZ:
        case Algorithm::LZMA: {
            impl_->xz_stream.next_in = data.data();
            impl_->xz_stream.avail_in = data.size();

            while (impl_->xz_stream.avail_in > 0) {
                impl_->xz_stream.next_out = impl_->output_buffer.data();
                impl_->xz_stream.avail_out = impl_->output_buffer.size();

                lzma_ret ret = lzma_code(&impl_->xz_stream, LZMA_RUN);
                if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
                    throw std::runtime_error("XZ decompression error");
                }

                size_t bytes_written = impl_->output_buffer.size() - impl_->xz_stream.avail_out;
                result.insert(result.end(), impl_->output_buffer.data(),
                              impl_->output_buffer.data() + bytes_written);

                if (ret == LZMA_STREAM_END) {
                    break;
                }
            }
            break;
        }
#endif
        default:
            throw std::invalid_argument("Unsupported decompression algorithm");
    }

    return result;
}

std::vector<uint8_t> DecompressStream::Finish() {
    if (impl_->finished) {
        throw std::runtime_error("Cannot finish: stream is already finished");
    }

    impl_->finished = true;

    // For most algorithms, finishing is implicit when all data has been processed
    // We just return empty and let the user verify completeness if needed
    return {};
}

bool DecompressStream::IsFinished() const {
    return impl_->finished;
}

Algorithm DecompressStream::algorithm() const {
    return impl_->algorithm;
}

}  // namespace compress_utils
