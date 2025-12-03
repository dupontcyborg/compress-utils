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
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB: {
            impl_->zlib_stream.next_in = const_cast<Bytef*>(data.data());
            impl_->zlib_stream.avail_in = static_cast<uInt>(data.size());

            while (impl_->zlib_stream.avail_in > 0) {
                impl_->zlib_stream.next_out = impl_->output_buffer.data();
                impl_->zlib_stream.avail_out = static_cast<uInt>(impl_->output_buffer.size());

                int ret = inflate(&impl_->zlib_stream, Z_NO_FLUSH);
                if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                    throw std::runtime_error("zlib decompression error");
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
