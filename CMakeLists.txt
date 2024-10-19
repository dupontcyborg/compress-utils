## Main CMake for `compression-utils` project

cmake_minimum_required(VERSION 3.17)

project(compression-utils)

######### PROJECT SETUP #########

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set default build type if not specified
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

######### COMPILER FLAGS #########

# Add LTO and Dead Code Elimination flags based on the compiler
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(LTO_FLAGS "-flto -ffunction-sections -fdata-sections")
    if(APPLE)
        set(LINKER_FLAGS "-Wl,-dead_strip")  # macOS specific
    else()
        set(LINKER_FLAGS "-Wl,--gc-sections")  # Linux/Unix
    endif()
elseif(MSVC)
    set(LTO_FLAGS "/GL")
    set(LINKER_FLAGS "/OPT:REF")
endif()

# C++ Flags
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${LTO_FLAGS} -O3")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")

# C Flags
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${LTO_FLAGS} -O3")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")

######### OPTIONS #########

# Option to enable/disable tests
option(ENABLE_TESTS "Enable building tests" ON)

# Options to include algorithms (set through build.sh)
option(INCLUDE_ZSTD "Include Zstd compression algorithm" ON)
option(INCLUDE_ZLIB "Include zlib compression algorithm" ON)

# Options to build bindings
option(BUILD_C_BINDINGS "Build C bindings" ON)

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

# Create library target (shared)
add_library(compress_utils SHARED)

# Create library target (static, used by bindings)
add_library(compress_utils_static STATIC)

######### ALGORITHMS #########

# Check if no algorithms are included, and throw an error
if (NOT INCLUDE_ZSTD AND NOT INCLUDE_ZLIB)
    message(FATAL_ERROR "No algorithms included. Please include at least one algorithm.")
endif()

# Dependency lists
set(ALGORITHMS_ENUM "")
set(TARGET_LIBS "")
set(TARGET_DEFINITIONS "")

# Zstd Algorithm
if (INCLUDE_ZSTD)
    message(STATUS "Including Zstd algorithm")
    # Add the subdirectory for zstd which contains its own CMakeLists.txt
    add_subdirectory(algorithms/zstd)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/libzstd.a)
    # Define preprocessor directive to include Zstd in the code
    list(APPEND TARGET_DEFINITIONS INCLUDE_ZSTD)
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
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/libz.a)
    # Define preprocessor directive to include zlib in the code
    list(APPEND TARGET_DEFINITIONS INCLUDE_ZLIB)
    # Add source files for zlib
    file(GLOB ZLIB_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/zlib/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/zlib/*.hpp")
    list(APPEND ALL_SOURCE_FILES ${ZLIB_SOURCE_FILES})
    # Add zlib to the enum
    list(APPEND ALGORITHMS_ENUM "ZLIB")
endif()

# Apply the libraries and definitions to both libraries
target_link_libraries(compress_utils PRIVATE ${TARGET_LIBS})
target_link_libraries(compress_utils_static PRIVATE ${TARGET_LIBS})
target_compile_definitions(compress_utils PRIVATE ${TARGET_DEFINITIONS})
target_compile_definitions(compress_utils_static PRIVATE ${TARGET_DEFINITIONS})

# TODO: for static lib, try `whole-archive` options so it includes all the symbols BUT also while supporting LTO/DCE

######### MAIN LIBRARY #########

# Add source files to the dynamic and static libraries
target_sources(compress_utils PRIVATE ${ALL_SOURCE_FILES})
target_sources(compress_utils_static PRIVATE ${ALL_SOURCE_FILES})

# Define COMPRESSION_UTILS_EXPORTS for the dynamic library
target_compile_definitions(compress_utils PRIVATE COMPRESSION_UTILS_EXPORTS)

######### INSTALL #########

# Set the output directory for the library and header files
set(CPP_DIST_LIB_DIR "${CMAKE_SOURCE_DIR}/dist/cpp/lib")
set(STATIC_LIB_DIR "${CMAKE_SOURCE_DIR}/dist/cpp/lib/static")
set(CPP_DIST_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/dist/cpp/include/compression_utils")

# Install libraries (dynamic and static)
install(TARGETS compress_utils compress_utils_static
    LIBRARY DESTINATION ${CPP_DIST_LIB_DIR}  # Dynamic lib
    ARCHIVE DESTINATION ${STATIC_LIB_DIR}  # Static lib
)

# Install the headers
install(FILES ${CMAKE_SOURCE_DIR}/src/compression_utils.hpp
              ${CMAKE_SOURCE_DIR}/src/compression_utils_func.hpp
              ${CMAKE_BINARY_DIR}/algorithms.hpp
        DESTINATION ${CPP_DIST_INCLUDE_DIR})

# Copy the README from bindings/cpp/README.md to the dist/cpp directory
install(FILES ${CMAKE_SOURCE_DIR}/bindings/cpp/README.md
        DESTINATION ${CMAKE_SOURCE_DIR}/dist/cpp)

# Copy the dependency libraries to the static directory
# Only if we are building the static lib
# TODO - remove this once we fix the `whole-archive` issue
if (TARGET compress_utils_static)
    install(FILES ${TARGET_LIBS}
            DESTINATION ${STATIC_LIB_DIR})
endif()

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

# Add tests if the option is enabled
if (ENABLE_TESTS)
    enable_testing()
    message(STATUS "Tests are enabled. Including the tests directory.")
    add_subdirectory(tests)
else()
    message(STATUS "Tests are disabled. Skipping the tests directory.")
endif()

######### BINDINGS #########

# C Bindings
if (BUILD_C_BINDINGS)
    message(STATUS "Building C binding...")
    add_subdirectory(bindings/c)

    # Ensure that the C bindings are built after the core C++ library
    add_dependencies(compress_utils_c compress_utils)
endif()

