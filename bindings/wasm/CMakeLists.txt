# compress-utils WASM build — child CMake project, one invocation per algo.
#
# Not invoked directly. The parent `BUILD_WASM_BINDINGS=ON` configure pulls
# in cmake/host.cmake (next to this file), which fans out via
# ExternalProject_Add: one child configure of this CMakeLists per entry in
# CU_WASM_ALGOS, each pinned to a single algorithm by -DCU_WASM_ALGO.
#
# Output: one or more direction variants per algorithm, post-processed
# (wasm-strip / wasm-opt) and staged under bindings/wasm/dist/algorithms/${ALGO}/:
#   both        -> <ALGO>/<ALGO>.wasm            (full compress + decompress)
#   decompress  -> <ALGO>/decompress/<ALGO>.wasm (decoder only)
#   compress    -> <ALGO>/compress/<ALGO>.wasm   (encoder only)
# The TypeScript dispatcher loads them on demand, one subpath import per
# (algo, direction), so bundlers tree-shake to exactly what's used.
#
# Upstream codec sources are pulled in by reusing the existing
# algorithms/${ALGO}/CMakeLists.txt subproject. Its ExternalProject_Add
# inherits CMAKE_TOOLCHAIN_FILE automatically, so the upstream lib is
# built for wasm32-wasi alongside ours, once, and shared by all variants.

cmake_minimum_required(VERSION 3.17)
# CXX is enabled because Snappy is a C++ codec; its wasm archive links the
# libc++ that the zig toolchain ships for wasm32-wasi. The C-only algorithms
# never touch the C++ compiler.
project(compress_utils_wasm C CXX)

if(NOT CU_WASM_TOOLCHAIN)
    message(FATAL_ERROR
        "Configure this project with --toolchain "
        "cmake/toolchains/zig-wasm.cmake")
endif()

set(CU_WASM_ALGO "zstd" CACHE STRING
    "Algorithm to compile into this .wasm (zstd|brotli|zlib|bz2|lz4|xz|snappy|gzip)")

set(_CU_VALID_ALGOS zstd brotli zlib bz2 lz4 xz snappy gzip)
if(NOT CU_WASM_ALGO IN_LIST _CU_VALID_ALGOS)
    message(FATAL_ERROR
        "CU_WASM_ALGO='${CU_WASM_ALGO}' is not one of: ${_CU_VALID_ALGOS}")
endif()

# Direction variants to build for this algo. Each entry produces one .wasm.
set(CU_WASM_DIR "both;decompress;compress" CACHE STRING
    "Direction variants to build (any of: both compress decompress)")
set(_CU_VALID_DIRS both compress decompress)
foreach(_d IN LISTS CU_WASM_DIR)
    if(NOT _d IN_LIST _CU_VALID_DIRS)
        message(FATAL_ERROR
            "CU_WASM_DIR entry '${_d}' is not one of: ${_CU_VALID_DIRS}")
    endif()
endforeach()

string(TOUPPER ${CU_WASM_ALGO} _CU_ALGO_UPPER)

set(CU_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# The codec library builder. cu_add_vendored_codec() (called by each
# algorithms/<algo>/CMakeLists.txt below) compiles the vendored sources in
# third_party/ for wasm32-wasi via this project's toolchain — no fetch, no
# per-target configure. Vendor.cmake reads third_party/ relative to
# CU_REPO_ROOT (set above), since this child project's CMAKE_SOURCE_DIR is the
# wasm dir, not the repo root.
include(${CU_REPO_ROOT}/cmake/Vendor.cmake)

# Match the root build's optimization shape.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -flto")

# --- Codec library from the vendored sources --------------------------------
# The algorithm subproject calls cu_add_vendored_codec() to build
# <algo>_library from third_party/ with this project's wasm toolchain. Built
# once here and linked by every direction variant below. Config is
# compile-time-portable, so wasm32-wasi needs no separate configure.
add_subdirectory(
    ${CU_REPO_ROOT}/algorithms/${CU_WASM_ALGO}
    ${CMAKE_BINARY_DIR}/algorithm
)

# Brotli embeds large lookup/dictionary tables by default. On wasm we compute
# them at load (BROTLI_STATIC_INIT_EARLY) to meet the .wasm size budget (~240 KB
# smaller). That path uses a load-time constructor, which the wasm reactor's
# _initialize runs and the clang toolchain supports — but MSVC cannot parse it,
# so it is applied only to the wasm build, not the shared manifest.
if(CU_WASM_ALGO STREQUAL "brotli")
    target_compile_definitions(brotli_library PRIVATE BROTLI_STATIC_INIT=1)
endif()

# --- Export allow-list groups -----------------------------------------------
# The toolchain dropped `-Wl,--export-dynamic`, so by default nothing but the
# reactor `_initialize` + `memory` is exported and wasm-ld GCs everything the
# module doesn't reference. We re-surface exactly the symbols the JS loader
# calls (see src/core/loader.ts WasmExports). Because an unexported symbol is
# not a GC root, a directional build that omits the compress (or decompress)
# ABI lets wasm-ld + wasm-opt DCE that whole direction's code and its upstream
# codec closure — a decompress-only zstd is ~1/4 the full module.
set(_CU_EXPORTS_COMMON
    cu_alloc cu_free
    cu_version cu_algorithm_available cu_last_error cu_strerror)
set(_CU_EXPORTS_COMPRESS
    cu_compress_bound cu_compress
    cu_compress_stream_create cu_compress_stream_write
    cu_compress_stream_finish cu_compress_stream_destroy)
set(_CU_EXPORTS_DECOMPRESS
    cu_decompress cu_decompress_size_hint cu_set_max_decompressed_size
    cu_decompress_stream_create cu_decompress_stream_write
    cu_decompress_stream_finish cu_decompress_stream_destroy)

find_program(WASM_STRIP wasm-strip)
find_program(WASM_OPT wasm-opt)
if(NOT WASM_STRIP)
    message(WARNING "wasm-strip not found; .wasm will retain debug info (~3x larger)")
endif()
if(NOT WASM_OPT)
    message(WARNING "wasm-opt not found; .wasm will not be optimized post-link")
endif()

# --- One module variant per direction ---------------------------------------
# Each variant compiles the same four sources and links the one shared codec
# lib, differing only in its export allow-list and the CU_OMIT_* defines that
# NULL the unused direction's vtable slots (see src/algorithms/*/*.c). Output:
#   both        -> dist/algorithms/<algo>/<algo>.wasm
#   <direction> -> dist/algorithms/<algo>/<direction>/<algo>.wasm
function(cu_add_wasm_variant DIR)
    if(DIR STREQUAL "both")
        set(_tgt ${CU_WASM_ALGO})
        set(_outdir ${CMAKE_CURRENT_SOURCE_DIR}/dist/algorithms/${CU_WASM_ALGO})
    else()
        set(_tgt ${CU_WASM_ALGO}_${DIR})
        set(_outdir ${CMAKE_CURRENT_SOURCE_DIR}/dist/algorithms/${CU_WASM_ALGO}/${DIR})
    endif()

    add_executable(${_tgt}
        ${CU_REPO_ROOT}/src/compress_utils.c
        ${CU_REPO_ROOT}/src/registry.c
        ${CU_REPO_ROOT}/src/algorithms/${CU_WASM_ALGO}/${CU_WASM_ALGO}.c
        ${CU_REPO_ROOT}/src/wasm_runtime.c
    )
    # Codec headers come from the linked <algo>_library's PUBLIC include dirs.
    target_include_directories(${_tgt} PRIVATE
        ${CU_REPO_ROOT}/include
        ${CU_REPO_ROOT}/src
    )
    # Only the chosen algorithm's INCLUDE_<X> is defined → registry.c switch
    # only compiles in this one case.
    target_compile_definitions(${_tgt} PRIVATE
        INCLUDE_${_CU_ALGO_UPPER}
        CU_BUILD_SHARED
    )
    # Passed in by host.cmake from the parent's PROJECT_VERSION_FROM_GIT.
    if(CU_BUILD_VERSION)
        target_compile_definitions(${_tgt} PRIVATE
            CU_BUILD_VERSION="${CU_BUILD_VERSION}")
    endif()
    # XZ needs LZMA_API_STATIC.
    if(CU_WASM_ALGO STREQUAL "xz")
        target_compile_definitions(${_tgt} PRIVATE LZMA_API_STATIC)
    endif()

    # Direction-specific export allow-list + vtable gating. Pruning the export
    # roots alone doesn't shed the unused direction (each codec's vtable
    # statically references both directions); CU_OMIT_* leaves those slots NULL
    # so LTO + GC can drop the direction's code and its codec closure.
    set(_exports ${_CU_EXPORTS_COMMON})
    if(NOT DIR STREQUAL "decompress")
        list(APPEND _exports ${_CU_EXPORTS_COMPRESS})
    endif()
    if(NOT DIR STREQUAL "compress")
        list(APPEND _exports ${_CU_EXPORTS_DECOMPRESS})
    endif()
    foreach(_sym IN LISTS _exports)
        target_link_options(${_tgt} PRIVATE "-Wl,--export=${_sym}")
    endforeach()
    if(DIR STREQUAL "decompress")
        target_compile_definitions(${_tgt} PRIVATE CU_OMIT_COMPRESS)
    elseif(DIR STREQUAL "compress")
        target_compile_definitions(${_tgt} PRIVATE CU_OMIT_DECOMPRESS)
    endif()

    # One vendored static lib per codec (brotli's enc/dec/common are combined
    # into brotli_library). CMake orders the build via the link dependency —
    # there are no ExternalProject fetch steps to depend on anymore.
    target_link_libraries(${_tgt} PRIVATE ${CU_WASM_ALGO}_library)

    # Snappy is C++; link with the C++ driver so the zig toolchain pulls in
    # libc++ for wasm32-wasi. The sources are C, so LTO stays coherent.
    if(CU_WASM_ALGO STREQUAL "snappy")
        set_target_properties(${_tgt} PROPERTIES LINKER_LANGUAGE CXX)
    endif()

    # Strip the .wasm suffix that CMake/WASI sets by default, then re-add it
    # explicitly so the output is `${algo}.wasm` regardless of generator. All
    # variants share OUTPUT_NAME, so give each its own build subdir to avoid
    # three targets racing to write the same `${algo}.wasm`.
    set_target_properties(${_tgt} PROPERTIES
        SUFFIX ".wasm"
        OUTPUT_NAME ${CU_WASM_ALGO}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${DIR}
    )

    # Stage + post-process (strip + wasm-opt -Oz). -Oz over -O3: with the
    # export allow-list pruned, wasm-ld GC leaves unreferenced codec functions
    # only this final pass can DCE; -Oz's DCE matches -O3's and just prefers
    # size on the survivors (perf validated via the benchmark suite).
    set(_out ${_outdir}/${CU_WASM_ALGO}.wasm)
    set(_cmds
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_outdir}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${_tgt}> ${_out}
    )
    if(WASM_STRIP)
        list(APPEND _cmds COMMAND ${WASM_STRIP} ${_out})
    endif()
    if(WASM_OPT)
        list(APPEND _cmds
            COMMAND ${WASM_OPT} -Oz
                --enable-bulk-memory --enable-sign-ext
                --enable-nontrapping-float-to-int --enable-mutable-globals
                ${_out} -o ${_out}.opt
            COMMAND ${CMAKE_COMMAND} -E rename ${_out}.opt ${_out}
        )
    endif()
    add_custom_command(TARGET ${_tgt} POST_BUILD ${_cmds}
        COMMENT "Staging ${CU_WASM_ALGO}.wasm (${DIR}) into dist/")
endfunction()

foreach(_dir IN LISTS CU_WASM_DIR)
    cu_add_wasm_variant(${_dir})
endforeach()
