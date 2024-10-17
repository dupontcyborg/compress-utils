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

# Library directories for algorithms (dist/lib is where the built libraries for each algorithm are stored)
set(ALGORITHMS_LIB_DIR ${CMAKE_SOURCE_DIR}/algorithms/dist/lib)

######### SOURCE FILES #########

# Get all the files from the src/ directory
file(GLOB_RECURSE ALL_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.hpp")

# Remove files from src/algorithms directory
file(GLOB_RECURSE ALGO_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/*.hpp")
list(REMOVE_ITEM ALL_SOURCE_FILES ${ALGO_SOURCE_FILES})

# Create library target
add_library(compress_utils SHARED)

######### ALGORITHMS #########

# Check if no algorithms are included, and throw an error
if (NOT INCLUDE_ZSTD AND NOT INCLUDE_ZLIB)
    message(FATAL_ERROR "No algorithms included. Please include at least one algorithm.")
endif()

# Enum for algorithms
set(ALGORITHMS_ENUM "")

# Zstd Algorithm
if (INCLUDE_ZSTD)
    message(STATUS "Including Zstd algorithm")
    # Add the subdirectory for zstd which contains its own CMakeLists.txt
    add_subdirectory(algorithms/zstd)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    target_link_libraries(compress_utils PRIVATE ${ALGORITHMS_LIB_DIR}/libzstd.a)
    # Define preprocessor directive to include Zstd in the code
    target_compile_definitions(compress_utils PRIVATE INCLUDE_ZSTD)
    # Add source files for Zstd
    file(GLOB ZSTD_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/zstd/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/zstd/*.hpp")
    list(APPEND ALL_SOURCE_FILES ${ZSTD_SOURCE_FILES})
    # Add Zstd to the enum
    list(APPEND ALGORITHMS_ENUM "ZSTD")
endif()

# zlib Algorithm
if (INCLUDE_ZLIB)
    message(STATUS "Including zlib algorithm")
    # Add the subdirectory for zlib which contains its own CMakeLists.txt
    add_subdirectory(algorithms/zlib)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    target_link_libraries(compress_utils PRIVATE ${ALGORITHMS_LIB_DIR}/libz.a)
    # Define preprocessor directive to include zlib in the code
    target_compile_definitions(compress_utils PRIVATE INCLUDE_ZLIB)
    # Add source files for zlib
    file(GLOB ZLIB_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/zlib/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/zlib/*.hpp")
    list(APPEND ALL_SOURCE_FILES ${ZLIB_SOURCE_FILES})
    # Add zlib to the enum
    list(APPEND ALGORITHMS_ENUM "ZLIB")
endif()

######### MAIN LIBRARY #########

# Build compress_utils as a shared (dynamic) library
target_sources(compress_utils PRIVATE ${ALL_SOURCE_FILES})

# Define COMPRESSION_UTILS_EXPORTS for the build
target_compile_definitions(compress_utils PRIVATE COMPRESSION_UTILS_EXPORTS)


######### INSTALL #########

# Set the output directory for the library and header files
set(CPP_DIST_LIB_DIR "${CMAKE_SOURCE_DIR}/dist/cpp/lib")
set(CPP_DIST_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dist/cpp/include/compression_utils")

# Copy the library after building it
add_custom_command(TARGET compress_utils POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CPP_DIST_LIB_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:compress_utils> ${CPP_DIST_LIB_DIR}
)

# Copy the headers after the library is built
add_custom_command(TARGET compress_utils POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CPP_DIST_INCLUDE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/src/compression_utils.hpp ${CPP_DIST_INCLUDE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/src/compression_utils_func.hpp ${CPP_DIST_INCLUDE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/algorithms.hpp ${CPP_DIST_INCLUDE_DIR}  # Use the processed version of algorithms.hpp
)

# Configure the header with enum
string(JOIN ",\n    " ALGORITHM_LIST ${ALGORITHMS_ENUM})

# Prepend string with four spaces
set(ALGORITHM_LIST "    ${ALGORITHM_LIST}")

# Configure the header file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/algorithms.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/algorithms.hpp
    @ONLY
)

######### TESTS #########

# if (ENABLE_TESTS)
#     message(STATUS "Building tests")
#     enable_testing()
#     add_subdirectory(tests)
# endif()

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

######### INSTALL #########

# Future: Add install targets