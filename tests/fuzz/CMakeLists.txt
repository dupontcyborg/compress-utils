## Fuzzing targets — opt-in via -DENABLE_FUZZ=ON.
##
## Requires clang with -fsanitize=fuzzer support. Typical configure:
##   CC=clang cmake -B build-fuzz -DENABLE_FUZZ=ON -DCMAKE_BUILD_TYPE=Debug
##
## After build, run:
##   ./build-fuzz/tests/fuzz/fuzz_decompress -max_total_time=60

if(NOT ENABLE_FUZZ)
    return()
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(WARNING "ENABLE_FUZZ requires Clang; skipping fuzz targets.")
    return()
endif()

set(FUZZ_FLAGS "-fsanitize=fuzzer,address,undefined")

add_executable(fuzz_decompress fuzz_decompress.c)
target_link_libraries(fuzz_decompress PRIVATE compress_utils_static)
target_include_directories(fuzz_decompress PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(fuzz_decompress PRIVATE ${FUZZ_FLAGS})
target_link_options(fuzz_decompress PRIVATE ${FUZZ_FLAGS})

if(INCLUDE_ZSTD)
    target_link_libraries(fuzz_decompress PRIVATE zstd_library)
endif()
if(INCLUDE_ZLIB)
    target_link_libraries(fuzz_decompress PRIVATE zlib_library)
endif()
if(INCLUDE_BROTLI)
    target_link_libraries(fuzz_decompress PRIVATE brotlienc brotlidec brotlicommon)
endif()
if(INCLUDE_BZ2)
    target_link_libraries(fuzz_decompress PRIVATE bz2_library)
endif()
if(INCLUDE_LZ4)
    target_link_libraries(fuzz_decompress PRIVATE lz4_library)
endif()
if(INCLUDE_XZ)
    target_link_libraries(fuzz_decompress PRIVATE xz_library)
endif()
