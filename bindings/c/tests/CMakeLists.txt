## CMake for C Binding Tests

######### EXTERNAL PROJECT #########

# Download and build CUnit using ExternalProject_Add
include(ExternalProject)
ExternalProject_Add(
    CUnit
    PREFIX ${CMAKE_BINARY_DIR}/external/CUnit
    GIT_REPOSITORY https://gitlab.com/cunity/cunit.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
    LOG_DOWNLOAD OFF LOG_UPDATE OFF LOG_CONFIGURE OFF LOG_BUILD OFF LOG_INSTALL OFF
)

# Specify the include and library directories for CUnit
ExternalProject_Get_Property(CUnit install_dir)
set(CUNIT_INCLUDE_DIR ${install_dir}/include)
set(CUNIT_LIBRARY_DIR ${install_dir}/lib)

######### TEST SETUP #########

# Define output directories for test binaries
set(TEST_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bindings/c/build/tests")

# Get all test files
file(GLOB TEST_FILES "*.c")

# Add test executables
add_executable(run_tests_c ${TEST_FILES})
add_executable(run_tests_c_static ${TEST_FILES})

# Add include directories to both executables
target_include_directories(run_tests_c PRIVATE ${CUNIT_INCLUDE_DIR} "../")
target_include_directories(run_tests_c_static PRIVATE ${CUNIT_INCLUDE_DIR} "../")

# Link libraries to both executables
add_dependencies(run_tests_c CUnit compress_utils_c)
target_link_libraries(run_tests_c PRIVATE ${CUNIT_LIBRARY_DIR}/libcunit.a compress_utils_c)
add_dependencies(run_tests_c_static CUnit compress_utils_c_static)
target_link_libraries(run_tests_c_static PRIVATE ${CUNIT_LIBRARY_DIR}/libcunit.a compress_utils_c_static)

# Set output directories for test executables
set(TARGET_PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
)
set_target_properties(run_tests_c PROPERTIES ${TARGET_PROPERTIES})
set_target_properties(run_tests_c_static PROPERTIES ${TARGET_PROPERTIES})

# Set compression preprocessor definitions
set(COMPRESSION_DEFINITIONS
    $<$<BOOL:${INCLUDE_BROTLI}>:INCLUDE_BROTLI>
    $<$<BOOL:${INCLUDE_XZ}>:INCLUDE_XZ>
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)
target_compile_definitions(run_tests_c PRIVATE ${COMPRESSION_DEFINITIONS})
target_compile_definitions(run_tests_c_static PRIVATE ${COMPRESSION_DEFINITIONS})

# Add tests to CTest
add_test(NAME run_tests_c COMMAND ${TEST_OUTPUT_DIR}/run_tests_c)
add_test(NAME run_tests_c_static COMMAND ${TEST_OUTPUT_DIR}/run_tests_c_static)