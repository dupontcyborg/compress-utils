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
    CMAKE_ARGS
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DCUNIT_BUILD_CU_TEST=OFF
        -DBUILD_SHARED_LIBS=OFF
    BUILD_COMMAND ${CMAKE_COMMAND} --build . --config Release
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
add_executable(unit_tests_c ${TEST_FILES})
add_executable(unit_tests_c_static ${TEST_FILES})

# Add include directories to both executables
target_include_directories(unit_tests_c PRIVATE ${CUNIT_INCLUDE_DIR} "../")
target_include_directories(unit_tests_c_static PRIVATE ${CUNIT_INCLUDE_DIR} "../")

# Handle Windows-specific library naming
if (WIN32)
    set(CUNIT_LIB "cunit.lib")
else()
    set(CUNIT_LIB "libcunit.a")
endif()

# Link libraries to both executables
add_dependencies(unit_tests_c CUnit compress_utils_c)
target_link_libraries(unit_tests_c PRIVATE ${CUNIT_LIBRARY_DIR}/${CUNIT_LIB} compress_utils_c)
add_dependencies(unit_tests_c_static CUnit compress_utils_c_static)
target_link_libraries(unit_tests_c_static PRIVATE ${CUNIT_LIBRARY_DIR}/${CUNIT_LIB} compress_utils_c_static)

# Set output directories for test executables
set(TARGET_PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
)
set_target_properties(unit_tests_c PROPERTIES ${TARGET_PROPERTIES})
set_target_properties(unit_tests_c_static PROPERTIES ${TARGET_PROPERTIES})

target_compile_definitions(unit_tests_c PRIVATE COMPRESS_UTILS_C_EXPORT_SHARED)
target_compile_definitions(unit_tests_c_static PRIVATE COMPRESS_UTILS_C_EXPORT_STATIC)

# Specify runtime library settings on Windows
if (WIN32)
    target_link_options(unit_tests_static PRIVATE /WHOLEARCHIVE:compress_utils_static)
    add_custom_command(TARGET unit_tests_c POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:compress_utils_c>
        $<TARGET_FILE_DIR:unit_tests_c>
    )
    add_custom_command(TARGET unit_tests_c POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:compress_utils>
        $<TARGET_FILE_DIR:unit_tests_c>
    )
endif()

# Add tests to CTest
add_test(NAME unit_tests_c COMMAND ${TEST_OUTPUT_DIR}/unit_tests_c)
add_test(NAME unit_tests_c_static COMMAND ${TEST_OUTPUT_DIR}/unit_tests_c_static)