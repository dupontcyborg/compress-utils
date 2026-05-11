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
# Consume the OBJECT library: pulls in the wrapper code as .o files and
# propagates the algorithm-library link deps via INTERFACE_LINK_LIBRARIES.
# Result: a self-contained exe — no DLL hunt at runtime.
target_link_libraries(test_compress_utils PRIVATE compress_utils_obj)

if(ENABLE_TESTS)
    add_test(NAME test_compress_utils COMMAND test_compress_utils)
endif()

add_subdirectory(fuzz)
