## Main CMake for `compress-utils` project.
##
## C-as-core. C++ binding, Python binding, and WASM binding all consume
## the C ABI declared in include/compress_utils.h.
##
## NOTE: this is a Phase 1 in-progress state. Only ZSTD is fully migrated
## to the C core. Other algorithms (zlib, brotli, bz2, lz4, xz) will be
## enabled as they get ported. Until then the per-algorithm options
## default OFF for the un-ported algorithms.

cmake_minimum_required(VERSION 3.17)

# Default version (used when git tag is not available)
set(DEFAULT_VERSION "0.1.0")

# Include git version extraction module
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake)
get_version_from_git(PROJECT_VERSION_FROM_GIT ${DEFAULT_VERSION})

project(compress-utils
    VERSION ${PROJECT_VERSION_FROM_GIT}
    LANGUAGES C CXX
)

######### PROJECT SETUP #########

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# C++ stays available for tests and (eventually) the C++ binding.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Hidden visibility by default — only CU_API symbols are exported.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Generate compile_commands.json for clangd / IDEs / fuzzers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    message(STATUS "CMAKE_BUILD_PARALLEL_LEVEL: $ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
endif()

######### COMPILER FLAGS #########

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(OPT_FLAGS "-flto -ffunction-sections -fdata-sections")
    if(APPLE)
        set(LINKER_FLAGS "-Wl,-dead_strip")
    else()
        set(LINKER_FLAGS "-Wl,--gc-sections")
    endif()
elseif(MSVC)
    set(OPT_FLAGS "/GL")
    set(LINKER_FLAGS "/OPT:REF")
endif()

set(CMAKE_EXE_LINKER_FLAGS_RELEASE    "${CMAKE_EXE_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} ${LINKER_FLAGS}")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} ${OPT_FLAGS} -O3")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${OPT_FLAGS} -O3")
elseif(MSVC)
    set(CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE} ${OPT_FLAGS} /O2")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${OPT_FLAGS} /O2")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

######### OPTIONS #########

option(ENABLE_TESTS "Enable building tests" ON)

# Per-algorithm inclusion. As of Phase 1, only ZSTD has been migrated to
# the C core. The others remain default-OFF until their ports land.
option(INCLUDE_ZSTD   "Include Zstd compression algorithm"   ON)
option(INCLUDE_BROTLI "Include Brotli compression algorithm" OFF)
option(INCLUDE_ZLIB   "Include zlib compression algorithm"   OFF)
option(INCLUDE_BZ2    "Include bzip2 compression algorithm"  OFF)
option(INCLUDE_LZ4    "Include LZ4 compression algorithm"    OFF)
option(INCLUDE_XZ     "Include XZ/LZMA compression algorithm" OFF)

if(NOT INCLUDE_ZSTD AND NOT INCLUDE_BROTLI AND NOT INCLUDE_ZLIB
        AND NOT INCLUDE_BZ2 AND NOT INCLUDE_LZ4 AND NOT INCLUDE_XZ)
    message(FATAL_ERROR "No algorithms included. Enable at least one INCLUDE_<ALGO>.")
endif()

######### LIBRARY TARGETS #########

# Core sources: ABI dispatcher + algorithm registry. Per-algorithm sources
# get appended below by their respective subdir blocks.
set(CU_CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/compress_utils.c
    ${CMAKE_SOURCE_DIR}/src/registry.c
)

set(CU_TARGET_DEFINITIONS "")
set(CU_TARGET_LIBS "")

# Algorithm fetch + per-algorithm vtable source. Each enabled algorithm
# pulls in the upstream library (still via the existing algorithms/<algo>/
# CMakeLists.txt that uses ExternalProject_Add) and adds its .c file to
# the core sources.
if(INCLUDE_ZSTD)
    message(STATUS "Including ZSTD")
    add_subdirectory(algorithms/zstd)
    list(APPEND CU_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/algorithms/zstd/zstd.c)
    list(APPEND CU_TARGET_LIBS zstd_library)
    list(APPEND CU_TARGET_DEFINITIONS INCLUDE_ZSTD)
endif()
if(INCLUDE_BROTLI)
    message(STATUS "Including Brotli")
    add_subdirectory(algorithms/brotli)
    list(APPEND CU_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/algorithms/brotli/brotli.c)
    list(APPEND CU_TARGET_LIBS brotlienc brotlidec brotlicommon)
    list(APPEND CU_TARGET_DEFINITIONS INCLUDE_BROTLI)
endif()
if(INCLUDE_ZLIB)
    message(STATUS "Including zlib")
    add_subdirectory(algorithms/zlib)
    list(APPEND CU_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/algorithms/zlib/zlib.c)
    list(APPEND CU_TARGET_LIBS zlib_library)
    list(APPEND CU_TARGET_DEFINITIONS INCLUDE_ZLIB)
endif()
if(INCLUDE_BZ2)
    message(STATUS "Including bzip2")
    add_subdirectory(algorithms/bz2)
    list(APPEND CU_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/algorithms/bz2/bz2.c)
    list(APPEND CU_TARGET_LIBS bz2_library)
    list(APPEND CU_TARGET_DEFINITIONS INCLUDE_BZ2)
endif()
if(INCLUDE_LZ4)
    message(STATUS "Including LZ4")
    add_subdirectory(algorithms/lz4)
    list(APPEND CU_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/algorithms/lz4/lz4.c)
    list(APPEND CU_TARGET_LIBS lz4_library)
    list(APPEND CU_TARGET_DEFINITIONS INCLUDE_LZ4)
endif()
if(INCLUDE_XZ)
    message(STATUS "Including XZ")
    add_subdirectory(algorithms/xz)
    list(APPEND CU_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/algorithms/xz/xz.c)
    list(APPEND CU_TARGET_LIBS xz_library)
    list(APPEND CU_TARGET_DEFINITIONS INCLUDE_XZ LZMA_API_STATIC)
endif()

# Windows-specific link deps preserved from the C++ build.
if(WIN32)
    find_package(Threads REQUIRED)
    list(APPEND CU_TARGET_LIBS "legacy_stdio_definitions" Threads::Threads "msvcrt")
endif()

# Library targets — both shared and static.
add_library(compress_utils        SHARED ${CU_CORE_SOURCES})
add_library(compress_utils_static STATIC ${CU_CORE_SOURCES})

foreach(tgt compress_utils compress_utils_static)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/algorithms/dist/include
    )
    target_compile_definitions(${tgt} PRIVATE ${CU_TARGET_DEFINITIONS})
    target_link_libraries(${tgt} PRIVATE ${CU_TARGET_LIBS})
endforeach()

# Shared library export macros — define CU_BUILD_SHARED on the shared
# target so CU_API expands to __declspec(dllexport)/visibility("default").
target_compile_definitions(compress_utils PRIVATE CU_BUILD_SHARED)

# Algorithm subprojects use ExternalProject_Add and need the fetch to
# complete before our sources compile.
if(INCLUDE_ZSTD)
    add_dependencies(compress_utils zstd_external zstd_library)
    add_dependencies(compress_utils_static zstd_external zstd_library)
endif()
if(INCLUDE_BROTLI)
    add_dependencies(compress_utils brotli_external brotlienc brotlidec brotlicommon)
    add_dependencies(compress_utils_static brotli_external brotlienc brotlidec brotlicommon)
endif()
if(INCLUDE_ZLIB)
    add_dependencies(compress_utils zlib_external zlib_library)
    add_dependencies(compress_utils_static zlib_external zlib_library)
endif()
if(INCLUDE_BZ2)
    add_dependencies(compress_utils bz2_external bz2_library)
    add_dependencies(compress_utils_static bz2_external bz2_library)
endif()
if(INCLUDE_LZ4)
    add_dependencies(compress_utils lz4_external lz4_library)
    add_dependencies(compress_utils_static lz4_external lz4_library)
endif()
if(INCLUDE_XZ)
    add_dependencies(compress_utils xz_external xz_library)
    add_dependencies(compress_utils_static xz_external xz_library)
endif()

######### INSTALL #########

option(SCIKIT_BUILD "Build within scikit-build environment" OFF)

set(CU_DIST_DIR "${CMAKE_SOURCE_DIR}/dist/c")

if(NOT SCIKIT_BUILD)
    install(TARGETS compress_utils
        LIBRARY DESTINATION ${CU_DIST_DIR}/lib
        ARCHIVE DESTINATION ${CU_DIST_DIR}/lib
        RUNTIME DESTINATION ${CU_DIST_DIR}/bin
    )
    install(TARGETS compress_utils_static
        ARCHIVE DESTINATION ${CU_DIST_DIR}/lib
    )
    install(FILES ${CMAKE_SOURCE_DIR}/include/compress_utils.h
            DESTINATION ${CU_DIST_DIR}/include)
endif()

######### TESTS #########

if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

######### BINDINGS (deferred to Phase 4) #########

# The C++ binding (bindings/cpp/), Python binding (bindings/python/), and
# WASM binding (bindings/wasm/) will be retargeted to consume the C ABI
# directly in Phase 4. Until then they are not built — they reference
# headers that no longer exist in the new layout.
