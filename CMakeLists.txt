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

# Load codec-versions.json. Exposes <ALGO>_URL / <ALGO>_TAG variables for
# the per-algorithm ExternalProject_Add calls. Single source of truth
# shared with tools/sync-codecs.py (which feeds bindings/zig/build.zig.zon).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CodecVersions.cmake)

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

# Build everything position-independent. The static archive
# (compress_utils_static) gets linked into the Python pybind11 module —
# itself a shared library — and on Linux the linker rejects TLS access
# patterns (R_X86_64_TPOFF32 against g_last_error) from non-PIC code
# embedded into a shared object. macOS Mach-O is permissive here, which
# is why this only surfaces on Linux CI.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
option(ENABLE_FUZZ  "Enable libFuzzer harnesses (clang only)" OFF)

# Per-algorithm inclusion. All six have been migrated to the C core in
# Phase 1; default to ON. Disable individually for slimmer builds.
option(INCLUDE_ZSTD   "Include Zstd compression algorithm"   ON)
option(INCLUDE_BROTLI "Include Brotli compression algorithm" ON)
option(INCLUDE_ZLIB   "Include zlib compression algorithm"   ON)
option(INCLUDE_BZ2    "Include bzip2 compression algorithm"  ON)
option(INCLUDE_LZ4    "Include LZ4 compression algorithm"    ON)
option(INCLUDE_XZ     "Include XZ/LZMA compression algorithm" ON)
option(INCLUDE_SNAPPY "Include Snappy compression algorithm" ON)

if(NOT INCLUDE_ZSTD AND NOT INCLUDE_BROTLI AND NOT INCLUDE_ZLIB
        AND NOT INCLUDE_BZ2 AND NOT INCLUDE_LZ4 AND NOT INCLUDE_XZ
        AND NOT INCLUDE_SNAPPY)
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
if(INCLUDE_SNAPPY)
    message(STATUS "Including Snappy")
    add_subdirectory(algorithms/snappy)
    list(APPEND CU_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/algorithms/snappy/snappy.c)
    list(APPEND CU_TARGET_LIBS snappy_library)
    list(APPEND CU_TARGET_DEFINITIONS INCLUDE_SNAPPY)
    # Snappy is C++; its static archive references the C++ runtime. Our
    # library, tests, and fuzz targets link with the C driver, which does not
    # pull in libstdc++/libc++ automatically — add it explicitly. MSVC embeds
    # the dependency in the objects, so nothing extra is needed there.
    if(NOT MSVC)
        if(APPLE)
            list(APPEND CU_TARGET_LIBS c++)
        else()
            list(APPEND CU_TARGET_LIBS stdc++)
        endif()
    endif()
endif()

# Platform link deps.
if(WIN32)
    find_package(Threads REQUIRED)
    list(APPEND CU_TARGET_LIBS "legacy_stdio_definitions" Threads::Threads "msvcrt")
else()
    # Linux glibc puts log2/pow/etc. in libm, separate from libc. macOS libSystem
    # bundles them so this is a no-op there. Brotli/zlib/lz4/xz can pull from libm.
    list(APPEND CU_TARGET_LIBS m)
endif()

# ---------------------------------------------------------------------------
# Library targets.
#
# Layout: one OBJECT library (compress_utils_obj) compiles the source files
# once; the public SHARED library and the internal consumers (Python
# pybind11 module, C/fuzz tests) all consume those objects. There is no
# public static library:
#
#   - End users who want a shared lib: use `compress_utils`.
#   - End users who want a self-contained executable: build with
#     `-DBUILD_SHARED_LIBS=OFF` (CMake-native) or consume the OBJECT lib
#     directly from a sibling CMake project.
#   - The previous public `compress_utils_static.a` was only ~146 KB of
#     wrapper code — useless without manually linking 6+ algo .a files.
#     Removed.
#
# The OBJECT library is compiled with CU_BUILD_SHARED so its symbols
# carry dllexport/default visibility. They get exported by whichever
# shared lib they end up in (compress_utils.dll or compress_utils_py.pyd).
# ---------------------------------------------------------------------------

add_library(compress_utils_obj OBJECT ${CU_CORE_SOURCES})
target_include_directories(compress_utils_obj
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/algorithms/dist/include
)
target_compile_definitions(compress_utils_obj PRIVATE
    ${CU_TARGET_DEFINITIONS}
    CU_BUILD_SHARED
    # cu_version() reads this; carries the full git-describe tag including
    # any prerelease suffix (0.7.0-rc.1, 0.7.0-rc.1-3-gabcd, etc.).
    CU_BUILD_VERSION="${PROJECT_VERSION_FROM_GIT}"
)
# INTERFACE link deps propagate to consumers (the shared lib, Python
# module, tests) so they all pick up the algorithm static archives.
target_link_libraries(compress_utils_obj INTERFACE ${CU_TARGET_LIBS})

add_library(compress_utils SHARED $<TARGET_OBJECTS:compress_utils_obj>)
target_include_directories(compress_utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(compress_utils PRIVATE ${CU_TARGET_LIBS})

# Algorithm subprojects use ExternalProject_Add and need the fetch to
# complete before our sources compile.
if(INCLUDE_ZSTD)
    add_dependencies(compress_utils_obj zstd_external zstd_library)
endif()
if(INCLUDE_BROTLI)
    add_dependencies(compress_utils_obj brotli_external brotlienc brotlidec brotlicommon)
endif()
if(INCLUDE_ZLIB)
    add_dependencies(compress_utils_obj zlib_external zlib_library)
endif()
if(INCLUDE_BZ2)
    add_dependencies(compress_utils_obj bz2_external bz2_library)
endif()
if(INCLUDE_LZ4)
    add_dependencies(compress_utils_obj lz4_external lz4_library)
endif()
if(INCLUDE_XZ)
    add_dependencies(compress_utils_obj xz_external xz_library)
endif()
if(INCLUDE_SNAPPY)
    add_dependencies(compress_utils_obj snappy_external snappy_library)
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
    install(FILES ${CMAKE_SOURCE_DIR}/include/compress_utils.h
            DESTINATION ${CU_DIST_DIR}/include)
endif()

######### TESTS #########

if(ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

######### BINDINGS #########

option(BUILD_CPP_BINDINGS    "Build C++ header-only binding"        ON)
option(BUILD_PYTHON_BINDINGS "Build Python binding (via pybind11)"  ON)
option(BUILD_WASM_BINDINGS   "Build per-algorithm .wasm modules (requires zig)" OFF)

if(BUILD_CPP_BINDINGS)
    add_subdirectory(bindings/cpp)
endif()
if(BUILD_PYTHON_BINDINGS)
    add_subdirectory(bindings/python)
endif()
if(BUILD_WASM_BINDINGS)
    # Drives one child CMake configure per algorithm with the zig-wasm
    # toolchain. Cross-compile lives in its own world; native build is
    # untouched. See bindings/wasm/host.cmake.
    include(bindings/wasm/host.cmake)
endif()
