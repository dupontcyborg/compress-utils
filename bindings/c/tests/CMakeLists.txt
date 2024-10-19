## CMake for C Binding Tests

include(ExternalProject)

# Download and build CUnit using ExternalProject_Add
ExternalProject_Add(
    CUnit
    PREFIX ${CMAKE_BINARY_DIR}/external/CUnit
    GIT_REPOSITORY https://gitlab.com/cunity/cunit.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    LOG_DOWNLOAD OFF
    LOG_UPDATE OFF
    LOG_CONFIGURE OFF
    LOG_BUILD OFF
    LOG_INSTALL OFF
)

# Specify the include directory for CUnit headers
ExternalProject_Get_Property(CUnit install_dir)
set(CUNIT_INCLUDE_DIR ${install_dir}/include)
set(CUNIT_LIBRARY_DIR ${install_dir}/lib)

# Define output directories for test binary
set(TEST_RUNTIME_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bindings/c/build/tests")
set(TEST_BINARY_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bindings/c/build/tests")

# Get all test files
file(GLOB TEST_FILES "*.c")

# Add the test executables (dynamic & static)
add_executable(run_tests_c ${TEST_FILES})
add_executable(run_tests_c_static ${TEST_FILES})

# Add a dependency on the CUnit target
add_dependencies(run_tests_c CUnit)
add_dependencies(run_tests_c_static CUnit)

# Ensure that C binding tests are built after the C bindings
add_dependencies(run_tests_c compress_utils_c)
add_dependencies(run_tests_c_static compress_utils_c_static)

# Link the test executable to CUnit (use CUNIT_LIBRARY_DIR)
target_include_directories(run_tests_c PRIVATE ${CUNIT_INCLUDE_DIR} "../")
target_include_directories(run_tests_c_static PRIVATE ${CUNIT_INCLUDE_DIR} "../")
target_link_libraries(run_tests_c PRIVATE ${CUNIT_LIBRARY_DIR}/libcunit.a compress_utils_c)
target_link_libraries(run_tests_c_static PRIVATE ${CUNIT_LIBRARY_DIR}/libcunit.a compress_utils_c_static)

# Specify the output directories for the test binary
set_target_properties(run_tests_c PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${TEST_RUNTIME_OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${TEST_BINARY_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${TEST_BINARY_OUTPUT_DIR}
)
set_target_properties(run_tests_c_static PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${TEST_RUNTIME_OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${TEST_BINARY_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${TEST_BINARY_OUTPUT_DIR}
)

# Pass the preprocessor definitions (like INCLUDE_ZLIB, INCLUDE_ZSTD) from the parent
target_compile_definitions(run_tests_c PRIVATE
    $<$<BOOL:${INCLUDE_BROTLI}>:INCLUDE_BROTLI>
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)
target_compile_definitions(run_tests_c_static PRIVATE
    $<$<BOOL:${INCLUDE_BROTLI}>:INCLUDE_BROTLI>
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)

# Add test to CTest
add_test(NAME run_tests_c COMMAND ${TEST_RUNTIME_OUTPUT_DIR}/run_tests_c)
add_test(NAME run_tests_c_static COMMAND ${TEST_RUNTIME_OUTPUT_DIR}/run_tests_c_static)