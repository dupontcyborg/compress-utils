## CMake for Main C++ Tests

######### EXTERNAL PROJECT #########

# Add an external project to download and build GoogleTest
include(ExternalProject)
ExternalProject_Add(
    googletest
    PREFIX ${CMAKE_BINARY_DIR}/external/gtest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG origin/main
    GIT_SHALLOW TRUE
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
    LOG_DOWNLOAD OFF LOG_UPDATE OFF LOG_CONFIGURE OFF LOG_BUILD OFF LOG_INSTALL OFF
    INSTALL_COMMAND ""
)

# Get source and binary directories for GoogleTest
ExternalProject_Get_Property(googletest source_dir)
ExternalProject_Get_Property(googletest binary_dir)
set(GTEST_INCLUDE_DIRS ${source_dir}/googletest/include)
set(GTEST_LIBS ${binary_dir}/lib/libgtest.a ${binary_dir}/lib/libgtest_main.a)

######### TEST SETUP #########

# Add test executables
file(GLOB TEST_FILES "*.cpp")
add_executable(run_tests ${TEST_FILES})
add_executable(run_tests_static ${TEST_FILES})

# Link libraries to both executables
add_dependencies(run_tests googletest compress_utils)
target_link_libraries(run_tests PRIVATE compress_utils ${GTEST_LIBS})
add_dependencies(run_tests_static googletest compress_utils_static)
target_link_libraries(run_tests_static PRIVATE compress_utils_static ${GTEST_LIBS})

# Add include directories to both executables
set(COMMON_INCLUDES ${CMAKE_SOURCE_DIR}/src ${GTEST_INCLUDE_DIRS})
target_include_directories(run_tests PRIVATE ${COMMON_INCLUDES})
target_include_directories(run_tests_static PRIVATE ${COMMON_INCLUDES})

# Apply preprocessor definitions for compression libraries
set(COMPRESSION_DEFINITIONS
    $<$<BOOL:${INCLUDE_BROTLI}>:INCLUDE_BROTLI>
    $<$<BOOL:${INCLUDE_XZ}>:INCLUDE_XZ>
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)
target_compile_definitions(run_tests PRIVATE ${COMPRESSION_DEFINITIONS})
target_compile_definitions(run_tests_static PRIVATE ${COMPRESSION_DEFINITIONS})

######### TEST REGISTRATION #########

# Enable testing and register both executables
enable_testing()
add_test(NAME run_tests COMMAND run_tests)
add_test(NAME run_tests_static COMMAND run_tests_static)