# tests/CMakeLists.txt

include(ExternalProject)

# Define where to build and install GoogleTest
set(GTEST_INSTALL_DIR "${CMAKE_BINARY_DIR}/external/gtest_install")

# Add an external project to download and build GoogleTest
ExternalProject_Add(
    googletest
    PREFIX ${CMAKE_BINARY_DIR}/external/gtest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG origin/main
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${GTEST_INSTALL_DIR} -DCMAKE_BUILD_TYPE=Release
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

target_link_libraries(run_tests
    PRIVATE
    ${GTEST_INSTALL_DIR}/lib/libgtest.a
    ${GTEST_INSTALL_DIR}/lib/libgtest_main.a
    compress_utils
)

# Ensure the test executable knows where to find the compression_utils headers
target_include_directories(run_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${GTEST_INSTALL_DIR}/include  # Add the gtest include directory
)

# Pass the preprocessor definitions (like INCLUDE_ZLIB, INCLUDE_ZSTD) from the parent
target_compile_definitions(run_tests PRIVATE
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)

# Enable testing and register the tests
enable_testing()
add_test(NAME run_tests COMMAND run_tests)