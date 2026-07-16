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

# Snappy differential test vs the reference google/snappy (C++). google/snappy
# is compiled HERE ONLY (from third_party/snappy-oracle) — it never enters the
# release library, which uses the pure-C andikleen port. This is a C++ target on
# purpose; the whole point of the swap is that C++ stays confined to tests.
if(ENABLE_TESTS AND INCLUDE_SNAPPY)
    set(_CU_SNAPPY_ORACLE ${CMAKE_SOURCE_DIR}/third_party/snappy-oracle)
    add_executable(test_snappy_oracle
        interop/snappy_oracle_test.cc
        ${_CU_SNAPPY_ORACLE}/snappy.cc
        ${_CU_SNAPPY_ORACLE}/snappy-sinksource.cc
        ${_CU_SNAPPY_ORACLE}/snappy-stubs-internal.cc)
    set_target_properties(test_snappy_oracle PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(test_snappy_oracle PRIVATE HAVE_CONFIG_H)
    target_include_directories(test_snappy_oracle PRIVATE
        ${_CU_SNAPPY_ORACLE} ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(test_snappy_oracle PRIVATE compress_utils_obj)
    # Silence google/snappy upstream warnings — we don't own that code.
    if(MSVC)
        target_compile_options(test_snappy_oracle PRIVATE /w)
    else()
        target_compile_options(test_snappy_oracle PRIVATE -w)
    endif()
    add_test(NAME test_snappy_oracle COMMAND test_snappy_oracle)
endif()

add_subdirectory(fuzz)
