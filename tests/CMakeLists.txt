## CMake for Main C++ Tests

include(ExternalProject)

# Add an external project to download and build GoogleTest
ExternalProject_Add(
    googletest
    PREFIX ${CMAKE_BINARY_DIR}/external/gtest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG origin/main
    GIT_SHALLOW TRUE
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
    BUILD_IN_SOURCE 0
    INSTALL_COMMAND ""
    LOG_DOWNLOAD OFF
    LOG_UPDATE OFF
    LOG_CONFIGURE OFF
    LOG_BUILD OFF
    LOG_INSTALL OFF
)

# Get all test files
file(GLOB TEST_FILES "*.cpp")

# Add the test executables (dynamic & static)
add_executable(run_tests ${TEST_FILES})
add_executable(run_tests_static ${TEST_FILES})

# Link compress_utils and the built GoogleTest libraries
add_dependencies(run_tests googletest)
add_dependencies(run_tests_static googletest)

# Get the source and binary directories for GoogleTest
ExternalProject_Get_Property(googletest source_dir)
ExternalProject_Get_Property(googletest binary_dir)
set(GTEST_INCLUDE_DIRS ${source_dir}/googletest/include)
set(GTEST_LIBS_DIR ${binary_dir}/lib)

# Link the test executables to GoogleTest & compress_utils
target_link_libraries(run_tests
    PRIVATE
    ${GTEST_LIBS_DIR}/libgtest.a
    ${GTEST_LIBS_DIR}/libgtest_main.a
    compress_utils
)
target_link_libraries(run_tests_static
    PRIVATE
    ${GTEST_LIBS_DIR}/libgtest.a
    ${GTEST_LIBS_DIR}/libgtest_main.a
    compress_utils_static
)

# Add the include directories to the test executables
target_include_directories(run_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GTEST_INCLUDE_DIRS}
)
target_include_directories(run_tests_static
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GTEST_INCLUDE_DIRS}
)

# Pass the preprocessor definitions (like INCLUDE_ZLIB, INCLUDE_ZSTD) from the parent
target_compile_definitions(run_tests PRIVATE
    $<$<BOOL:${INCLUDE_BROTLI}>:INCLUDE_BROTLI>
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)
target_compile_definitions(run_tests_static PRIVATE
    $<$<BOOL:${INCLUDE_BROTLI}>:INCLUDE_BROTLI>
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)

# Enable testing and register the tests
enable_testing()
add_test(NAME run_tests COMMAND run_tests)
add_test(NAME run_tests_static COMMAND run_tests_static)