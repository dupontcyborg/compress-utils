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
    LOG_DOWNLOAD OFF     # Suppress download logs
    LOG_UPDATE OFF       # Suppress update logs
    LOG_CONFIGURE OFF    # Suppress configure logs
    LOG_BUILD OFF        # Suppress build logs
    LOG_INSTALL OFF      # Suppress install logs
)

# Get all test files
file(GLOB TEST_FILES "*.cpp")

# Add the test executable
add_executable(run_tests ${TEST_FILES})

# Link compress_utils and the built GoogleTest libraries
add_dependencies(run_tests googletest)

ExternalProject_Get_Property(googletest source_dir)
ExternalProject_Get_Property(googletest binary_dir)
set(GTEST_INCLUDE_DIRS ${source_dir}/googletest/include)
set(GTEST_LIBS_DIR ${binary_dir}/lib)

target_link_libraries(run_tests
    PRIVATE
    ${GTEST_LIBS_DIR}/libgtest.a
    ${GTEST_LIBS_DIR}/libgtest_main.a
    compress_utils
)

# Ensure the test executable knows where to find the compression_utils headers
target_include_directories(run_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GTEST_INCLUDE_DIRS}  # Add the gtest include directory
)

# Pass the preprocessor definitions (like INCLUDE_ZLIB, INCLUDE_ZSTD) from the parent
target_compile_definitions(run_tests PRIVATE
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)

# Enable testing and register the tests
enable_testing()
add_test(NAME run_tests COMMAND run_tests)