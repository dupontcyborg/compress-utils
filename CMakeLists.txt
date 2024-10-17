## Main CMake for `compression-utils` project

cmake_minimum_required(VERSION 3.17)

project(compression-utils)

######### PROJECT SETUP #########

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

######### OPTIONS #########

# Option to enable/disable tests
option(ENABLE_TESTS "Enable building tests" ON)

# Options to include algorithms (set through build.sh)
option(INCLUDE_ZSTD "Include Zstd compression algorithm" ON)
option(INCLUDE_ZLIB "Include zlib compression algorithm" ON)

######### INCLUDE DIRECTORIES #########

# Include main src directory
include_directories(${CMAKE_SOURCE_DIR}/src)

# Include directories for algorithms (dist/include is where headers for each algorithm are stored)
include_directories(${CMAKE_SOURCE_DIR}/algorithms/dist/include)

######### SOURCE FILES #########

# Add all source files from src/ directory
file(GLOB_RECURSE COMPRESSION_UTILS_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.hpp")

######### MAIN LIBRARY #########

# Add the main library target for compress_utils, linking all the source files in src/
add_library(compress_utils STATIC ${COMPRESSION_UTILS_SOURCES})

######### ALGORITHMS #########

# Zstd Algorithm
if (INCLUDE_ZSTD)
    message(STATUS "Including Zstd algorithm")
    # Add the subdirectory for zstd which contains its own CMakeLists.txt
    add_subdirectory(algorithms/zstd)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    target_link_libraries(compress_utils PRIVATE zstd_library)
    # Define preprocessor directive to include Zstd in the code
    target_compile_definitions(compress_utils PRIVATE INCLUDE_ZSTD)
endif()

# zlib Algorithm
if (INCLUDE_ZLIB)
    message(STATUS "Including zlib algorithm")
    # Add the subdirectory for zlib which contains its own CMakeLists.txt
    add_subdirectory(algorithms/zlib)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    target_link_libraries(compress_utils PRIVATE zlib_library)
    # Define preprocessor directive to include zlib in the code
    target_compile_definitions(compress_utils PRIVATE INCLUDE_ZLIB)
endif()

######### TESTS #########

if (ENABLE_TESTS)
    message(STATUS "Building tests")
    enable_testing()
    add_subdirectory(tests)
endif()

######### BINDINGS #########

# Placeholder for language bindings setup
# Example:
# if (BUILD_JS_BINDINGS)
#     add_subdirectory(bindings/js)
# endif()
# if (BUILD_PYTHON_BINDINGS)
#     add_subdirectory(bindings/python)
# endif()

######### COPY OUTPUTS #########

# # Set output directories
# set(LIB_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/dist/cpp/lib)
# set(INCLUDE_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/dist/cpp/include/compression-utils)

# # Copy the output libraries to the output directory
# add_custom_command(TARGET compress_utils POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E make_directory ${LIB_OUTPUT_DIR}
#     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:compress_utils> ${LIB_OUTPUT_DIR}
# )

# # Copy the header files to the output directory ()
# add_custom_command(TARGET compress_utils POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E make_directory ${INCLUDE_OUTPUT_DIR}
#     COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/src ${INCLUDE_OUTPUT_DIR}
# )