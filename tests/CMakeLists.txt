## C test suite for compress-utils.
##
## test_compress_utils.c exercises the public C ABI in include/compress_utils.h:
##   - version + algorithm introspection
##   - one-shot compress/decompress across all six algorithms with five
##     input sizes (empty / single byte / repeating text / random 1MB /
##     repetitive 1MB) — though sizes are reduced for the C suite; the
##     1MB cases live in the Python suite.
##   - cu_decompress_size_hint probe behavior (CU_OK on algos that
##     encode size; CU_ERR_SIZE_UNKNOWN on those that don't).
##   - CU_ERR_BUF_TOO_SMALL with required-size reporting (zstd).
##   - Streaming round-trip with a 256-byte tight buffer, forcing dozens
##     of BUF_TOO_SMALL drain cycles per pass — this is the test that
##     would catch the unconsumed-input bug from the legacy C++ code.
##   - Cross-API: stream-compress → one-shot decompress (and vice versa)
##     across all algorithms — catches wire-format mismatches.
##
## Fuzzing (tests/fuzz/) is gated by -DENABLE_FUZZ=ON.

add_executable(test_compress_utils test_compress_utils.c)
target_link_libraries(test_compress_utils PRIVATE compress_utils_static)
target_include_directories(test_compress_utils PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Static link of the algorithm libs needs explicit link order.
if(INCLUDE_ZSTD)
    target_link_libraries(test_compress_utils PRIVATE zstd_library)
endif()
if(INCLUDE_ZLIB)
    target_link_libraries(test_compress_utils PRIVATE zlib_library)
endif()
if(INCLUDE_BROTLI)
    target_link_libraries(test_compress_utils PRIVATE brotlienc brotlidec brotlicommon)
endif()
if(INCLUDE_BZ2)
    target_link_libraries(test_compress_utils PRIVATE bz2_library)
endif()
if(INCLUDE_LZ4)
    target_link_libraries(test_compress_utils PRIVATE lz4_library)
endif()
if(INCLUDE_XZ)
    target_link_libraries(test_compress_utils PRIVATE xz_library)
endif()

if(WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_compress_utils PRIVATE Threads::Threads)
else()
    # Linux libm for math symbols that the algorithm libs reach for.
    target_link_libraries(test_compress_utils PRIVATE m)
endif()

if(ENABLE_TESTS)
    add_test(NAME test_compress_utils COMMAND test_compress_utils)
endif()

add_subdirectory(fuzz)
