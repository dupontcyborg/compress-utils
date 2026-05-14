# compress-utils WASM build — child CMake project, one invocation per algo.
#
# Not invoked directly. The parent `BUILD_WASM_BINDINGS=ON` configure pulls
# in cmake/host.cmake (next to this file), which fans out via
# ExternalProject_Add: one child configure of this CMakeLists per entry in
# CU_WASM_ALGOS, each pinned to a single algorithm by -DCU_WASM_ALGO.
#
# Output: <build>/${ALGO}.wasm — one self-contained module per algorithm,
# then post-processed (wasm-strip / wasm-opt) and staged into
# bindings/wasm/dist/algorithms/${ALGO}/. The TypeScript dispatcher loads
# them on demand, one subpath import per algo, so bundlers can tree-shake
# aggressively.
#
# Upstream codec sources are pulled in by reusing the existing
# algorithms/${ALGO}/CMakeLists.txt subproject. Its ExternalProject_Add
# inherits CMAKE_TOOLCHAIN_FILE automatically, so the upstream lib is
# built for wasm32-wasi alongside ours. No duplicated fetch logic.

cmake_minimum_required(VERSION 3.17)
project(compress_utils_wasm C)

if(NOT CU_WASM_TOOLCHAIN)
    message(FATAL_ERROR
        "Configure this project with --toolchain "
        "cmake/toolchains/zig-wasm.cmake")
endif()

set(CU_WASM_ALGO "zstd" CACHE STRING
    "Algorithm to compile into this .wasm (zstd|brotli|zlib|bz2|lz4|xz)")

set(_CU_VALID_ALGOS zstd brotli zlib bz2 lz4 xz)
if(NOT CU_WASM_ALGO IN_LIST _CU_VALID_ALGOS)
    message(FATAL_ERROR
        "CU_WASM_ALGO='${CU_WASM_ALGO}' is not one of: ${_CU_VALID_ALGOS}")
endif()

string(TOUPPER ${CU_WASM_ALGO} _CU_ALGO_UPPER)

set(CU_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Load the codec-versions.json manifest. The wasm child CMake project
# isn't a subdir of the root, so we have to include this explicitly —
# otherwise ${<ALGO>_URL} / ${<ALGO>_TAG} expand to empty in the algo
# subprojects we add_subdirectory below.
include(${CU_REPO_ROOT}/cmake/CodecVersions.cmake)

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

# --- Upstream codec via the existing subproject -----------------------------
# CU_ALGO_STAGING_DIR tells the algorithm subproject where to fetch and
# stage its upstream library. We use a path under CMAKE_BINARY_DIR so the
# wasm cross-compile's artifacts don't collide with a native build's
# source-tree cache at ${CMAKE_SOURCE_DIR}/algorithms/dist.
set(CU_ALGO_STAGING_DIR "${CMAKE_BINARY_DIR}/algorithms")
add_subdirectory(
    ${CU_REPO_ROOT}/algorithms/${CU_WASM_ALGO}
    ${CMAKE_BINARY_DIR}/algorithm
)

# --- compress-utils wasm module ---------------------------------------------
add_executable(${CU_WASM_ALGO}
    ${CU_REPO_ROOT}/src/compress_utils.c
    ${CU_REPO_ROOT}/src/registry.c
    ${CU_REPO_ROOT}/src/algorithms/${CU_WASM_ALGO}/${CU_WASM_ALGO}.c
    ${CU_REPO_ROOT}/src/wasm_runtime.c
)

target_include_directories(${CU_WASM_ALGO} PRIVATE
    ${CU_REPO_ROOT}/include
    ${CU_REPO_ROOT}/src
    ${CU_ALGO_STAGING_DIR}/dist/include
)

# Only the chosen algorithm's INCLUDE_<X> is defined → registry.c switch
# only compiles in this one case. Other algorithm .c files are not in
# the source list at all, so the upstream lib for them is never fetched.
target_compile_definitions(${CU_WASM_ALGO} PRIVATE
    INCLUDE_${_CU_ALGO_UPPER}
    CU_BUILD_SHARED
)

# XZ needs LZMA_API_STATIC.
if(CU_WASM_ALGO STREQUAL "xz")
    target_compile_definitions(${CU_WASM_ALGO} PRIVATE LZMA_API_STATIC)
endif()

if(CU_WASM_ALGO STREQUAL "brotli")
    # Brotli ships as three archives, not one.
    target_link_libraries(${CU_WASM_ALGO} PRIVATE brotlienc brotlidec brotlicommon)
    add_dependencies(${CU_WASM_ALGO} brotli_external)
else()
    target_link_libraries(${CU_WASM_ALGO} PRIVATE ${CU_WASM_ALGO}_library)
    add_dependencies(${CU_WASM_ALGO} ${CU_WASM_ALGO}_external)
endif()

# Strip the .wasm suffix that CMake/WASI sets by default, then re-add it
# explicitly so the output is `${algo}.wasm` regardless of generator.
set_target_properties(${CU_WASM_ALGO} PROPERTIES
    SUFFIX ".wasm"
    OUTPUT_NAME ${CU_WASM_ALGO}
)

# --- Stage + post-process output into bindings/wasm/dist/algorithms/${ALGO}/
#
# Raw zig output for zstd is ~3.6 MB, with ~2.6 MB of that in DWARF debug
# sections. wasm-strip drops debug to ~600 KB; wasm-opt -O3 brings it
# further to ~550 KB. Both tools are optional — if they're not on PATH we
# ship the unstripped artifact and emit a warning so the build still works.
set(CU_WASM_DIST ${CMAKE_CURRENT_SOURCE_DIR}/dist/algorithms/${CU_WASM_ALGO})
set(CU_WASM_OUT ${CU_WASM_DIST}/${CU_WASM_ALGO}.wasm)

find_program(WASM_STRIP wasm-strip)
find_program(WASM_OPT wasm-opt)

set(_CU_STAGE_CMDS
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CU_WASM_DIST}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${CU_WASM_ALGO}> ${CU_WASM_OUT}
)
if(WASM_STRIP)
    list(APPEND _CU_STAGE_CMDS COMMAND ${WASM_STRIP} ${CU_WASM_OUT})
else()
    message(WARNING "wasm-strip not found; .wasm will retain debug info (~3x larger)")
endif()
if(WASM_OPT)
    list(APPEND _CU_STAGE_CMDS
        COMMAND ${WASM_OPT} -O3
            --enable-bulk-memory --enable-sign-ext
            --enable-nontrapping-float-to-int --enable-mutable-globals
            ${CU_WASM_OUT} -o ${CU_WASM_OUT}.opt
        COMMAND ${CMAKE_COMMAND} -E rename ${CU_WASM_OUT}.opt ${CU_WASM_OUT}
    )
else()
    message(WARNING "wasm-opt not found; .wasm will not be optimized post-link")
endif()

add_custom_command(TARGET ${CU_WASM_ALGO} POST_BUILD
    ${_CU_STAGE_CMDS}
    COMMENT "Staging ${CU_WASM_ALGO}.wasm into dist/ (strip + opt)"
)
