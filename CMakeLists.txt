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

# Set C and C++ Optimization Flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${LTO_FLAGS} -O3")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${LTO_FLAGS} -O3")
elseif(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${LTO_FLAGS} /O2")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${LTO_FLAGS} /O2")
endif()

######### OPTIONS #########

# Option to enable/disable tests
option(ENABLE_TESTS "Enable building tests" ON)

# Options to include algorithms (set through build.sh)
option(INCLUDE_BROTLI "Include Brotli compression algorithm" ON)
option(INCLUDE_XZ "Include XZ/LZMA compression algorithm" ON)
option(INCLUDE_ZSTD "Include Zstd compression algorithm" ON)
option(INCLUDE_ZLIB "Include zlib compression algorithm" ON)

# Options to build bindings
option(BUILD_C_BINDINGS "Build C bindings" ON)

######### INCLUDE DIRECTORIES #########

# Include main src & algorithms' headers
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/algorithms/dist/include)

# Library directories for algorithms (dist/lib is where the built libraries for each algorithm are stored)
set(ALGORITHMS_LIB_DIR ${CMAKE_SOURCE_DIR}/algorithms/dist/lib)

######### SOURCE FILES #########

# Get all the files from the src/ directory
file(GLOB_RECURSE ALL_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.hpp")

# Remove files from src/algorithms directory (to be re-added based on algorithm selection)
file(GLOB_RECURSE ALGO_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/*.hpp")
list(REMOVE_ITEM ALL_SOURCE_FILES ${ALGO_SOURCE_FILES})

# Create library targets (shared & static)
add_library(compress_utils SHARED)
add_library(compress_utils_static STATIC)

######### ALGORITHMS #########

# Check if no algorithms are included, and throw an error
if (NOT INCLUDE_BROTLI AND NOT INCLUDE_XZ AND NOT INCLUDE_ZSTD AND NOT INCLUDE_ZLIB)
    message(FATAL_ERROR "No algorithms included. Please include at least one algorithm.")
endif()

# Dependency lists
set(ALGORITHMS_ENUM "")
set(TARGET_LIBS "")
set(TARGET_DEFINITIONS "")

# Handle Windows-specific library naming
if (WIN32)
    set(BROTLI_LIB_ENC "brotlienc.lib")
    set(BROTLI_LIB_DEC "brotlidec.lib")
    set(BROTLI_LIB_COMMON "brotlicommon.lib")
    set(XZ_LIB "lzma.lib")
    set(ZLIB_LIB "zlib.lib")
    set(ZSTD_LIB "zstd_static.lib")

else()
    set(BROTLI_LIB_ENC "libbrotlienc.a")
    set(BROTLI_LIB_DEC "libbrotlidec.a")
    set(BROTLI_LIB_COMMON "libbrotlicommon.a")
    set(XZ_LIB "liblzma.a")
    set(ZLIB_LIB "libz.a")
    set(ZSTD_LIB "libzstd.a")
endif()

# Brotli Algorithm
if (INCLUDE_BROTLI)
    message(STATUS "Including Brotli algorithm")
    # Add the subdirectory for Brotli which contains its own CMakeLists.txt
    add_subdirectory(algorithms/brotli)
    # Ensure that Brotli is built before compress_utils
    add_dependencies(compress_utils brotli_external)
    add_dependencies(compress_utils_static brotli_external)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/${BROTLI_LIB_ENC})
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/${BROTLI_LIB_DEC})
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/${BROTLI_LIB_COMMON})
    # Define preprocessor directive to include Brotli in the code
    list(APPEND TARGET_DEFINITIONS INCLUDE_BROTLI)
    # Add source files for Brotli
    file(GLOB BROTLI_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/brotli/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/brotli/*.hpp")
    list(APPEND ALL_SOURCE_FILES ${BROTLI_SOURCE_FILES})
    # Add Brotli to the enum
    list(APPEND ALGORITHMS_ENUM "BROTLI")
endif()

# XZ/LZMA Algorithm
if (INCLUDE_XZ)
    message(STATUS "Including XZ/LZMA algorithm")
    # Add the subdirectory for XZ/LZMA which contains its own CMakeLists.txt
    add_subdirectory(algorithms/xz)
    # Ensure that xz is built before compress_utils
    add_dependencies(compress_utils xz_external)
    add_dependencies(compress_utils_static xz_external)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/${XZ_LIB})
    # Define preprocessor directive to include XZ/LZMA in the code
    list(APPEND TARGET_DEFINITIONS INCLUDE_XZ)
    # Add source files for XZ
    file(GLOB XZ_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/xz/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/xz/*.hpp")
    list(APPEND ALL_SOURCE_FILES ${XZ_SOURCE_FILES})
    # Add XZ & LZMA to the enum
    list(APPEND ALGORITHMS_ENUM "LZMA")
    list(APPEND ALGORITHMS_ENUM "XZ")
endif()


# zlib Algorithm
if (INCLUDE_ZLIB)
    message(STATUS "Including zlib algorithm")
    # Add the subdirectory for zlib which contains its own CMakeLists.txt
    add_subdirectory(algorithms/zlib)
    # Ensure that zlib is built before compress_utils
    add_dependencies(compress_utils zlib_external)
    add_dependencies(compress_utils_static zlib_external)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/${ZLIB_LIB})
    # Define preprocessor directive to include zlib in the code
    list(APPEND TARGET_DEFINITIONS INCLUDE_ZLIB)
    # Add source files for zlib
    file(GLOB ZLIB_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/zlib/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/zlib/*.hpp")
    list(APPEND ALL_SOURCE_FILES ${ZLIB_SOURCE_FILES})
    # Add zlib to the enum
    list(APPEND ALGORITHMS_ENUM "ZLIB")
endif()

# Zstd Algorithm
if (INCLUDE_ZSTD)
    message(STATUS "Including Zstd algorithm")
    # Add the subdirectory for zstd which contains its own CMakeLists.txt
    add_subdirectory(algorithms/zstd)
    # Ensure that Zstd is built before compress_utils
    add_dependencies(compress_utils zstd_external)
    add_dependencies(compress_utils_static zstd_external)
    # Ensure that the algorithm-specific libraries are linked to compress_utils
    list(APPEND TARGET_LIBS ${ALGORITHMS_LIB_DIR}/${ZSTD_LIB})
    # Define preprocessor directive to include Zstd in the code
    list(APPEND TARGET_DEFINITIONS INCLUDE_ZSTD)
    # Add source files for Zstd
    file(GLOB ZSTD_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/algorithms/zstd/*.cpp" "${CMAKE_SOURCE_DIR}/src/algorithms/zstd/*.hpp")
    list(APPEND ALL_SOURCE_FILES ${ZSTD_SOURCE_FILES})
    # Add Zstd to the enum
    list(APPEND ALGORITHMS_ENUM "ZSTD")
endif()

# Add Windows-specific dependencies
if(WIN32)
    list(APPEND TARGET_LIBS "legacy_stdio_definitions")
    
    # Add threading support
    find_package(Threads REQUIRED)
    list(APPEND TARGET_LIBS Threads::Threads)
    
    # Add math library
    list(APPEND TARGET_LIBS "msvcrt")
endif()

# Apply the libraries and definitions to both libraries
target_link_libraries(compress_utils PRIVATE ${TARGET_LIBS})
target_link_libraries(compress_utils_static PRIVATE ${TARGET_LIBS})
target_compile_definitions(compress_utils PRIVATE ${TARGET_DEFINITIONS})
target_compile_definitions(compress_utils_static PRIVATE ${TARGET_DEFINITIONS})

# TODO: for static lib, try `whole-archive` options so it includes all the symbols BUT also while supporting LTO/DCE

######### MAIN LIBRARY #########

# Add source files to the shared and static libraries
target_sources(compress_utils PRIVATE ${ALL_SOURCE_FILES})
target_sources(compress_utils_static PRIVATE ${ALL_SOURCE_FILES})

# Define symbol visibility for shared and static libraries
target_compile_definitions(compress_utils PRIVATE COMPRESSION_UTILS_EXPORT_SHARED)
target_compile_definitions(compress_utils_static PRIVATE COMPRESSION_UTILS_EXPORT_STATIC)

######### INSTALL #########

# Set the output directory for the library and header files
set(CPP_SHARED_DIR "${CMAKE_SOURCE_DIR}/dist/cpp/shared")
set(CPP_STATIC_DIR "${CMAKE_SOURCE_DIR}/dist/cpp/static")

# Install libraries (shared and static)
install(TARGETS compress_utils compress_utils_static
    LIBRARY DESTINATION ${CPP_SHARED_DIR}/lib  # Shared lib
    ARCHIVE DESTINATION ${CPP_STATIC_DIR}/lib  # Static lib
)

# Install the headers for the shared and static libraries
file(GLOB HEADERS ${CMAKE_SOURCE_DIR}/src/compression_utils.hpp
              ${CMAKE_SOURCE_DIR}/src/compression_utils_func.hpp
              ${CMAKE_CURRENT_BINARY_DIR}/algorithms.hpp)
install(FILES ${HEADERS}
        DESTINATION ${CPP_SHARED_DIR}/include)
install(FILES ${HEADERS}
        DESTINATION ${CPP_STATIC_DIR}/include)

# Install and rename the correct symbol_exports headers
install (FILES ${CMAKE_CURRENT_BINARY_DIR}/symbol_exports_shared.hpp
         DESTINATION ${CPP_SHARED_DIR}/include
            RENAME symbol_exports.hpp)
install (FILES ${CMAKE_CURRENT_BINARY_DIR}/symbol_exports_static.hpp
            DESTINATION ${CPP_STATIC_DIR}/include
                RENAME symbol_exports.hpp)

# Copy the README from bindings/cpp/README.md to the dist/cpp directory
install(FILES ${CMAKE_SOURCE_DIR}/bindings/cpp/README.md
        DESTINATION ${CMAKE_SOURCE_DIR}/dist/cpp)

# Copy the dependency libraries to the static directory
# TODO - remove this once we fix the `whole-archive` issue
if (TARGET compress_utils_static)
    install(FILES ${TARGET_LIBS} DESTINATION ${CPP_STATIC_DIR}/lib)
endif()

# Configure the header with enum
string(JOIN ",\n    " ALGORITHM_LIST ${ALGORITHMS_ENUM})
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
    add_dependencies(run_tests compress_utils)
    add_dependencies(run_tests_static compress_utils_static)
else()
    message(STATUS "Tests are disabled. Skipping the tests directory.")
endif()

######### WINDOWS LINKING #########

# Set runtime library flags for Windows targets
if (WIN32)
    set_target_properties(compress_utils PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    set_target_properties(compress_utils_static PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    # target_link_options(compress_utils_static PRIVATE /MT)
endif()

######### BINDINGS #########

# C Bindings
if (BUILD_C_BINDINGS)
    message(STATUS "Building C binding...")
    add_subdirectory(bindings/c)
    add_dependencies(compress_utils_c compress_utils)
endif()