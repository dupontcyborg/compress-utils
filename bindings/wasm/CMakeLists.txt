# CMakeLists.txt for compress-utils WASM bindings
#
# This builds WebAssembly modules for each compression algorithm using Emscripten.
# Use with: emcmake cmake .. && cmake --build .
#
# Prerequisites:
#   - Emscripten SDK installed and activated
#   - Run from bindings/wasm/build directory

cmake_minimum_required(VERSION 3.17)
project(compress-utils-wasm LANGUAGES C CXX)

# Verify we're building with Emscripten
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This CMakeLists.txt must be used with Emscripten. Run: emcmake cmake ..")
endif()

message(STATUS "Building compress-utils WASM modules with Emscripten")

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

######### OPTIONS #########

option(WASM_DEBUG "Build WASM modules with debug symbols" OFF)
option(INCLUDE_ZSTD "Build zstd WASM module" ON)
option(INCLUDE_BROTLI "Build brotli WASM module" ON)
option(INCLUDE_ZLIB "Build zlib WASM module" ON)
option(INCLUDE_BZ2 "Build bz2 WASM module" ON)
option(INCLUDE_LZ4 "Build lz4 WASM module" ON)
option(INCLUDE_XZ "Build xz WASM module" ON)

######### PATHS #########

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(WASM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(WASM_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/wasm-build")
set(ALGORITHMS_DIR "${PROJECT_ROOT}/algorithms")

# Create output directory
file(MAKE_DIRECTORY ${WASM_OUTPUT_DIR})

######### COMMON EMSCRIPTEN FLAGS #########

# Common flags for all WASM modules
set(WASM_COMMON_FLAGS
    "-s WASM=1"
    "-s MODULARIZE=1"
    "-s EXPORT_ES6=1"
    "-s ALLOW_MEMORY_GROWTH=1"
    "-s MALLOC=emmalloc"
    "-s FILESYSTEM=0"
    "-s ENVIRONMENT='web,node'"
    "-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','wasmMemory']"
    "-s SINGLE_FILE=1"
    "--no-entry"
)

# Exported functions (same for all algorithms)
set(WASM_EXPORTED_FUNCTIONS
    "_malloc"
    "_free"
    "_cu_compress"
    "_cu_decompress"
    "_cu_stream_compress_create"
    "_cu_stream_compress_write"
    "_cu_stream_compress_finish"
    "_cu_stream_compress_destroy"
    "_cu_stream_decompress_create"
    "_cu_stream_decompress_write"
    "_cu_stream_decompress_finish"
    "_cu_stream_decompress_destroy"
)

# Convert list to comma-separated string for Emscripten
string(REPLACE ";" "," EXPORTED_FUNCTIONS_STR "${WASM_EXPORTED_FUNCTIONS}")
list(APPEND WASM_COMMON_FLAGS "-s EXPORTED_FUNCTIONS='[${EXPORTED_FUNCTIONS_STR}]'")

# Debug or release flags
if(WASM_DEBUG)
    list(APPEND WASM_COMMON_FLAGS "-g" "-O0" "-s ASSERTIONS=2")
else()
    list(APPEND WASM_COMMON_FLAGS "-Oz" "-flto" "--closure 1")
endif()

# Convert flags list to string
string(REPLACE ";" " " WASM_LINK_FLAGS "${WASM_COMMON_FLAGS}")

######### FETCH DEPENDENCIES #########

include(FetchContent)

# Zstd
if(INCLUDE_ZSTD)
    FetchContent_Declare(
        zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(zstd)
endif()

# Brotli
if(INCLUDE_BROTLI)
    FetchContent_Declare(
        brotli
        GIT_REPOSITORY https://github.com/google/brotli.git
        GIT_TAG v1.1.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(brotli)
endif()

# Zlib
if(INCLUDE_ZLIB)
    FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(zlib)
endif()

# Bzip2 - needs special handling as it doesn't have CMakeLists.txt
if(INCLUDE_BZ2)
    FetchContent_Declare(
        bzip2
        GIT_REPOSITORY https://gitlab.com/bzip2/bzip2.git
        GIT_TAG bzip2-1.0.8
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(bzip2)
    if(NOT bzip2_POPULATED)
        FetchContent_Populate(bzip2)
        # Build bzip2 manually since it doesn't have CMake
        add_library(bz2_static STATIC
            ${bzip2_SOURCE_DIR}/blocksort.c
            ${bzip2_SOURCE_DIR}/huffman.c
            ${bzip2_SOURCE_DIR}/crctable.c
            ${bzip2_SOURCE_DIR}/randtable.c
            ${bzip2_SOURCE_DIR}/compress.c
            ${bzip2_SOURCE_DIR}/decompress.c
            ${bzip2_SOURCE_DIR}/bzlib.c
        )
        target_include_directories(bz2_static PUBLIC ${bzip2_SOURCE_DIR})
    endif()
endif()

# LZ4
if(INCLUDE_LZ4)
    FetchContent_Declare(
        lz4
        GIT_REPOSITORY https://github.com/lz4/lz4.git
        GIT_TAG v1.10.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake
    )
    set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(lz4)
endif()

# XZ/LZMA
if(INCLUDE_XZ)
    FetchContent_Declare(
        xz
        GIT_REPOSITORY https://github.com/tukaani-project/xz.git
        GIT_TAG v5.6.3
        GIT_SHALLOW TRUE
    )
    # XZ-specific options for WASM (no threads)
    set(ENABLE_THREADS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(xz)
endif()

######### WASM MODULE TARGETS #########

# Helper function to create a WASM module
function(add_wasm_module NAME)
    set(options "")
    set(oneValueArgs BINDING_CPP)
    set(multiValueArgs SOURCES INCLUDE_DIRS LINK_LIBS COMPILE_DEFS)
    cmake_parse_arguments(WASM "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    set(TARGET_NAME "${NAME}_wasm")
    set(OUTPUT_JS "${WASM_OUTPUT_DIR}/${NAME}.js")

    add_executable(${TARGET_NAME}
        ${WASM_BINDING_CPP}
        ${WASM_SOURCES}
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_ROOT}/src
        ${WASM_INCLUDE_DIRS}
    )

    if(WASM_COMPILE_DEFS)
        target_compile_definitions(${TARGET_NAME} PRIVATE ${WASM_COMPILE_DEFS})
    endif()

    if(WASM_LINK_LIBS)
        target_link_libraries(${TARGET_NAME} PRIVATE ${WASM_LINK_LIBS})
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME "${NAME}"
        SUFFIX ".js"
        RUNTIME_OUTPUT_DIRECTORY "${WASM_OUTPUT_DIR}"
        LINK_FLAGS "${WASM_LINK_FLAGS}"
    )

    message(STATUS "  Added WASM module: ${NAME}")
endfunction()

message(STATUS "Configuring WASM modules:")

# Zstd WASM module
if(INCLUDE_ZSTD)
    add_wasm_module(zstd
        BINDING_CPP "${WASM_SRC_DIR}/algorithms/zstd/binding.cpp"
        INCLUDE_DIRS "${zstd_SOURCE_DIR}/lib"
        LINK_LIBS libzstd_static
    )
endif()

# Brotli WASM module
if(INCLUDE_BROTLI)
    add_wasm_module(brotli
        BINDING_CPP "${WASM_SRC_DIR}/algorithms/brotli/binding.cpp"
        INCLUDE_DIRS "${brotli_SOURCE_DIR}/c/include"
        LINK_LIBS brotlienc brotlidec brotlicommon
    )
endif()

# Zlib WASM module
if(INCLUDE_ZLIB)
    add_wasm_module(zlib
        BINDING_CPP "${WASM_SRC_DIR}/algorithms/zlib/binding.cpp"
        INCLUDE_DIRS "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}"
        LINK_LIBS zlibstatic
    )
endif()

# Bz2 WASM module
if(INCLUDE_BZ2)
    add_wasm_module(bz2
        BINDING_CPP "${WASM_SRC_DIR}/algorithms/bz2/binding.cpp"
        INCLUDE_DIRS "${bzip2_SOURCE_DIR}"
        LINK_LIBS bz2_static
    )
endif()

# LZ4 WASM module
if(INCLUDE_LZ4)
    add_wasm_module(lz4
        BINDING_CPP "${WASM_SRC_DIR}/algorithms/lz4/binding.cpp"
        INCLUDE_DIRS "${lz4_SOURCE_DIR}/lib"
        LINK_LIBS lz4_static
    )
endif()

# XZ WASM module
if(INCLUDE_XZ)
    add_wasm_module(xz
        BINDING_CPP "${WASM_SRC_DIR}/algorithms/xz/binding.cpp"
        INCLUDE_DIRS "${xz_SOURCE_DIR}/src/liblzma/api"
        LINK_LIBS liblzma
    )
endif()

######### POST-BUILD: COPY TO SRC #########

# Custom target to copy generated JS files to src directories
add_custom_target(copy_wasm_modules ALL
    COMMENT "Copying WASM modules to source directories"
)

foreach(ALGO zstd brotli zlib bz2 lz4 xz)
    string(TOUPPER "INCLUDE_${ALGO}" INCLUDE_VAR)
    if(${${INCLUDE_VAR}})
        add_custom_command(TARGET copy_wasm_modules POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WASM_OUTPUT_DIR}/${ALGO}.js"
                "${WASM_SRC_DIR}/algorithms/${ALGO}/wasm.generated.js"
            COMMENT "Copying ${ALGO}.js to src"
            DEPENDS ${ALGO}_wasm
        )
        add_dependencies(copy_wasm_modules ${ALGO}_wasm)
    endif()
endforeach()

message(STATUS "WASM build configured successfully")
