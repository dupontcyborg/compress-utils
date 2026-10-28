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

# Concatenated zstd frames / gzip members and trailing data after zlib and raw
# DEFLATE streams. Skips any codec that isn't compiled in.
add_executable(test_concat_streams test_concat_streams.c)
target_link_libraries(test_concat_streams PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_concat_streams COMMAND test_concat_streams)
endif()

# Stream drain contract: nothing a codec could already emit is left inside it
# once a drained write returns. Skips any codec that isn't compiled in.
add_executable(test_stream_drain test_stream_drain.c)
target_link_libraries(test_stream_drain PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_stream_drain COMMAND test_stream_drain)
endif()

# Nonterminal flush: every checkpoint decodes completely from the bytes so
# far, on the same open stream. Skips any codec that isn't compiled in.
add_executable(test_stream_flush test_stream_flush.c)
target_link_libraries(test_stream_flush PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_stream_flush COMMAND test_stream_flush)
endif()

# Single-stream decoding (CU_DSTREAM_SINGLE): stops at the end of the first
# frame and hands back the input after it. Skips any codec that isn't compiled in.
add_executable(test_single_stream test_single_stream.c)
target_link_libraries(test_single_stream PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_single_stream COMMAND test_single_stream)
endif()

# Error and edge-case paths of the public API for every codec: undersized
# buffers, empty/truncated input, the size cap, stream misuse, data after the
# end. Skips any codec that isn't compiled in.
add_executable(test_error_paths test_error_paths.c)
target_link_libraries(test_error_paths PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_error_paths COMMAND test_error_paths)
endif()

# The advertised cu_compress_bound is a true upper bound for the DEFLATE family
# at every level and block type (stored, fixed, dynamic); exact-size output
# buffers must be enough. Skips any codec that isn't compiled in.
add_executable(test_deflate_bound test_deflate_bound.c)
target_link_libraries(test_deflate_bound PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_deflate_bound COMMAND test_deflate_bound)
endif()

# The 1..10 level scale of the DEFLATE family: level 1 is clearly the largest,
# level 7 beats level 2, and levels 8-10 are one setting (deflate_backend.h).
add_executable(test_deflate_levels test_deflate_levels.c)
target_link_libraries(test_deflate_levels PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_deflate_levels COMMAND test_deflate_levels)
endif()

# One-shot xz compression caps the dictionary at the input size: the header
# records it, round trips and far matches hold at the boundaries, the reference
# xz CLI decodes (skipped when absent), streaming keeps the preset's dictionary.
add_executable(test_xz_dict test_xz_dict.c)
target_link_libraries(test_xz_dict PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    add_test(NAME test_xz_dict COMMAND test_xz_dict)
endif()

# Snappy differential test vs the reference google/snappy (C++). google/snappy
# is compiled HERE ONLY (from third_party/snappy-oracle) — it never enters the
# release library, which uses our own pure-C core. This is a C++ target on
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

    # Differential test of the first-party Snappy core (src/algorithms/snappy/
    # core) against the same oracle: both directions on many inputs, and
    # accept/reject + byte-for-byte agreement on corrupted and generated ones,
    # with guard pages around every buffer. Links the core directly (not
    # compress_utils_obj), so the oracle's C++ symbols and ours never meet.
    set(_CU_SNAPPY_CORE ${CMAKE_SOURCE_DIR}/src/algorithms/snappy/core)
    add_executable(test_snappy_core_diff
        interop/snappy_core_diff_test.cc
        ${_CU_SNAPPY_CORE}/snappy_core.c
        ${_CU_SNAPPY_ORACLE}/snappy.cc
        ${_CU_SNAPPY_ORACLE}/snappy-sinksource.cc
        ${_CU_SNAPPY_ORACLE}/snappy-stubs-internal.cc)
    set_target_properties(test_snappy_core_diff PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(test_snappy_core_diff PRIVATE HAVE_CONFIG_H)
    target_include_directories(test_snappy_core_diff PRIVATE ${_CU_SNAPPY_ORACLE} ${_CU_SNAPPY_CORE})
    if(MSVC)
        set_source_files_properties(
            ${_CU_SNAPPY_ORACLE}/snappy.cc ${_CU_SNAPPY_ORACLE}/snappy-sinksource.cc
            ${_CU_SNAPPY_ORACLE}/snappy-stubs-internal.cc PROPERTIES COMPILE_OPTIONS /w)
    else()
        set_source_files_properties(
            ${_CU_SNAPPY_ORACLE}/snappy.cc ${_CU_SNAPPY_ORACLE}/snappy-sinksource.cc
            ${_CU_SNAPPY_ORACLE}/snappy-stubs-internal.cc PROPERTIES COMPILE_OPTIONS -w)
    endif()
    add_test(NAME test_snappy_core_diff COMMAND test_snappy_core_diff)
    set_tests_properties(test_snappy_core_diff PROPERTIES TIMEOUT 600)
endif()

add_subdirectory(fuzz)

if(INCLUDE_LZO)
    add_executable(test_lzo test_lzo.c)
    target_link_libraries(test_lzo PRIVATE compress_utils_obj)
    target_compile_definitions(test_lzo PRIVATE LZO_FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lzo")
    add_test(NAME test_lzo COMMAND test_lzo)
endif()

if(INCLUDE_LZO AND INCLUDE_LZ4)
    add_executable(test_raw_streams test_raw_streams.c)
    target_link_libraries(test_raw_streams PRIVATE compress_utils_obj)
    target_compile_definitions(test_raw_streams PRIVATE LZO_FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lzo")
    add_test(NAME test_raw_streams COMMAND test_raw_streams)
endif()
