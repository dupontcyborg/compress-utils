## Fuzzing targets.
##
## Always built: fuzz_replay, the harness plus replay_main.c, which runs the
## saved crash inputs in regressions/ once each (the fuzz_regressions ctest).
## Any compiler, any OS.
##
## Opt-in via -DENABLE_FUZZ=ON (clang with libFuzzer; on macOS use Homebrew's
## llvm, Apple's clang has no libFuzzer). The root CMakeLists.txt then
## instruments every target for coverage + ASan/UBSan. Typical configure:
##   CC=clang CXX=clang++ cmake -B build-fuzz -DENABLE_FUZZ=ON \
##       -DCMAKE_BUILD_TYPE=Debug -DBUILD_PYTHON_BINDINGS=OFF
##   cmake --build build-fuzz --target fuzz_decompress fuzz_roundtrip fuzz_seeds
##
## Then run (new corpus entries go to the first directory):
##   mkdir -p build-fuzz/corpus
##   ./build-fuzz/tests/fuzz/fuzz_decompress build-fuzz/corpus \
##       build-fuzz/tests/fuzz/seeds -max_total_time=60
##   (fuzz_roundtrip takes the same arguments; it fuzzes the encoders.)
##
## See DEVELOPMENT.md ("Fuzzing") for reproducing crashes and adding them to
## regressions/.

add_executable(fuzz_replay fuzz_decompress.c replay_main.c)
target_link_libraries(fuzz_replay PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    file(GLOB CU_FUZZ_REGRESSIONS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/regressions/*)
    list(FILTER CU_FUZZ_REGRESSIONS EXCLUDE REGEX "README\\.md$")
    add_test(NAME fuzz_regressions COMMAND fuzz_replay ${CU_FUZZ_REGRESSIONS})
endif()

# Same for the round-trip harness: regressions-roundtrip/ holds its saved inputs.
add_executable(fuzz_roundtrip_replay fuzz_roundtrip.c replay_main.c)
target_link_libraries(fuzz_roundtrip_replay PRIVATE compress_utils_obj)
if(ENABLE_TESTS)
    file(GLOB CU_FUZZ_RT_REGRESSIONS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/regressions-roundtrip/*)
    list(FILTER CU_FUZZ_RT_REGRESSIONS EXCLUDE REGEX "README\\.md$")
    add_test(NAME fuzz_roundtrip_regressions
        COMMAND fuzz_roundtrip_replay ${CU_FUZZ_RT_REGRESSIONS})
endif()

if(NOT ENABLE_FUZZ)
    return()
endif()

add_executable(fuzz_decompress fuzz_decompress.c)
target_link_libraries(fuzz_decompress PRIVATE compress_utils_obj)
target_link_options(fuzz_decompress PRIVATE -fsanitize=fuzzer)

# Compress side: random bytes must round-trip through every encoder, one-shot
# and streaming. Any file works as a seed (the decoder seeds below do).
add_executable(fuzz_roundtrip fuzz_roundtrip.c)
target_link_libraries(fuzz_roundtrip PRIVATE compress_utils_obj)
target_link_options(fuzz_roundtrip PRIVATE -fsanitize=fuzzer)

# Seed corpus: generated from the library itself into <build>/tests/fuzz/seeds,
# so no binary seeds are committed. `cmake --build <dir> --target fuzz_seeds`.
add_executable(gen_fuzz_seeds gen_seeds.c)
target_link_libraries(gen_fuzz_seeds PRIVATE compress_utils_obj)
set(CU_FUZZ_SEEDS ${CMAKE_CURRENT_BINARY_DIR}/seeds)
add_custom_target(fuzz_seeds
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CU_FUZZ_SEEDS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CU_FUZZ_SEEDS}
    COMMAND gen_fuzz_seeds ${CU_FUZZ_SEEDS} ${CMAKE_SOURCE_DIR}/tests/fixtures/lzo
    DEPENDS gen_fuzz_seeds
    COMMENT "Generating fuzz seed corpus in ${CU_FUZZ_SEEDS}"
    VERBATIM)
