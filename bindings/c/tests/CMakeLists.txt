## CMake for C Binding Tests

include(ExternalProject)

# Download and build CUnit using ExternalProject_Add
ExternalProject_Add(
    CUnit
    PREFIX ${CMAKE_BINARY_DIR}/external/CUnit
    GIT_REPOSITORY https://gitlab.com/cunit/CUnit.git
    GIT_TAG master
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
)

# Specify the include directory for CUnit headers
ExternalProject_Get_Property(CUnit install_dir)
set(CUNIT_INCLUDE_DIR ${install_dir}/include)
set(CUNIT_LIBRARY_DIR ${install_dir}/lib)

# Add a dependency on the CUnit target
add_dependencies(test_compression_utils_c CUnit)

# Add the test executable
add_executable(test_compression_utils_c test_compression_utils_c.c)

# Link the test executable to CUnit (use CUNIT_LIBRARY_DIR)
target_include_directories(test_compression_utils_c PRIVATE ${CUNIT_INCLUDE_DIR})
target_link_libraries(test_compression_utils_c PRIVATE ${CUNIT_LIBRARY_DIR}/libcunit.a)

# Add test to CTest
add_test(NAME test_compression_utils_c COMMAND test_compression_utils_c)

# TODO - make this be activate from the C bindings CMake if tests are enabled, and 