## CMake for Main C++ Tests

######### EXTERNAL PROJECT #########

message(STATUS "Setting up GoogleTest with Build Type: ${CMAKE_BUILD_TYPE}")

# Add an external project to download and build GoogleTest
include(ExternalProject)
ExternalProject_Add(
    googletest
    PREFIX ${CMAKE_BINARY_DIR}/external/gtest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG origin/main
    GIT_SHALLOW TRUE
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    LOG_DOWNLOAD OFF LOG_UPDATE OFF LOG_CONFIGURE OFF LOG_BUILD OFF LOG_INSTALL OFF
    INSTALL_COMMAND ""
)

# Handle Windows-specific library naming
if (WIN32)
    set(GTEST_LIB_DIR ${CMAKE_BUILD_TYPE}/)
    set(GTEST_LIB "gtest.lib")
    set(GTEST_LIB_MAIN "gtest_main.lib")
else()
    set(GTEST_LIB_DIR "")
    set(GTEST_LIB "libgtest.a")
    set(GTEST_LIB_MAIN "libgtest_main.a")
endif()

# Get source and binary directories for GoogleTest
ExternalProject_Get_Property(googletest source_dir)
ExternalProject_Get_Property(googletest binary_dir)
set(GTEST_INCLUDE_DIRS ${source_dir}/googletest/include)
set(GTEST_LIBS ${binary_dir}/lib/${GTEST_LIB_DIR}${GTEST_LIB} ${binary_dir}/lib/${GTEST_LIB_DIR}${GTEST_LIB_MAIN})

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

# Set library input directories of algorithms
set(COMPRESSION_LIBS ${CMAKE_SOURCE_DIR}/algorithms/dist/lib)
target_link_directories(run_tests PRIVATE ${COMPRESSION_LIBS})

# Apply preprocessor definitions for compression libraries
set(COMPRESSION_DEFINITIONS
    $<$<BOOL:${INCLUDE_BROTLI}>:INCLUDE_BROTLI>
    $<$<BOOL:${INCLUDE_XZ}>:INCLUDE_XZ>
    $<$<BOOL:${INCLUDE_ZLIB}>:INCLUDE_ZLIB>
    $<$<BOOL:${INCLUDE_ZSTD}>:INCLUDE_ZSTD>
)
target_compile_definitions(run_tests PRIVATE ${COMPRESSION_DEFINITIONS})
target_compile_definitions(run_tests_static PRIVATE ${COMPRESSION_DEFINITIONS})

target_compile_definitions(run_tests PRIVATE COMPRESSION_UTILS_EXPORT_SHARED)
target_compile_definitions(run_tests_static PRIVATE COMPRESSION_UTILS_EXPORT_STATIC)

######### WINDOWS LINKING #########

# Set runtime library flags for Windows test executables
if (WIN32)
    set_target_properties(run_tests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    set_target_properties(run_tests_static PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    # target_link_options(run_tests_static PRIVATE /MT)
    # target_link_libraries(run_tests_static PRIVATE ucrt.lib)
endif()

######### TEST REGISTRATION #########

# Enable testing and register both executables
enable_testing()
add_test(NAME run_tests COMMAND run_tests)
add_test(NAME run_tests_static COMMAND run_tests_static)