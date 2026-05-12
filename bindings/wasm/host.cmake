# Parent-side WASM driver.
#
# Included from the root CMakeLists.txt when -DBUILD_WASM_BINDINGS=ON.
# Spawns one ExternalProject_Add per algorithm, each a child CMake
# configure of bindings/wasm/CMakeLists.txt with the zig-wasm toolchain
# applied. Result: `cmake --build build` from a native configure builds
# the native shared lib AND all six .wasm artifacts in one pass.
#
# Why ExternalProject and not add_subdirectory: the WASM build needs a
# completely separate toolchain. add_subdirectory inherits the parent's
# compiler. ExternalProject runs a fresh CMake configure with its own
# toolchain — the same mechanism the algorithm subprojects already use
# for upstream codec sources.

include(ExternalProject)

set(CU_WASM_ALGOS_ALL zstd brotli zlib bz2 lz4 xz)

# Default the WASM algo set to whatever the parent CMake configure has
# enabled via INCLUDE_<ALGO>. Users can still pin a smaller subset
# explicitly via -DCU_WASM_ALGOS="zstd;brotli". Without this default,
# --algorithms=zstd --languages=wasm would try to fanout to all 6.
set(_CU_WASM_DEFAULT "")
foreach(_a IN LISTS CU_WASM_ALGOS_ALL)
    string(TOUPPER ${_a} _a_upper)
    if(INCLUDE_${_a_upper})
        list(APPEND _CU_WASM_DEFAULT ${_a})
    endif()
endforeach()
if(NOT _CU_WASM_DEFAULT)
    set(_CU_WASM_DEFAULT "${CU_WASM_ALGOS_ALL}")
endif()
set(CU_WASM_ALGOS "${_CU_WASM_DEFAULT}" CACHE STRING
    "Which algorithms to build as .wasm modules (subset of: ${CU_WASM_ALGOS_ALL})")
set(CU_WASM_BUILD_TYPE Release CACHE STRING
    "CMAKE_BUILD_TYPE for the wasm cross-compile (independent of the parent)")

set(_CU_WASM_TOOLCHAIN ${CMAKE_SOURCE_DIR}/cmake/toolchains/zig-wasm.cmake)
set(_CU_WASM_SRC ${CMAKE_SOURCE_DIR}/bindings/wasm)

add_custom_target(compress_utils_wasm)

foreach(ALGO IN LISTS CU_WASM_ALGOS)
    if(NOT ALGO IN_LIST CU_WASM_ALGOS_ALL)
        message(FATAL_ERROR "CU_WASM_ALGOS contains unknown algo '${ALGO}'")
    endif()

    set(_PROJ wasm_${ALGO})
    set(_PREFIX ${CMAKE_BINARY_DIR}/wasm-${ALGO})

    ExternalProject_Add(${_PROJ}
        PREFIX           ${_PREFIX}
        SOURCE_DIR       ${_CU_WASM_SRC}
        BINARY_DIR       ${_PREFIX}/build
        INSTALL_COMMAND  ""
        BUILD_ALWAYS     1
        CMAKE_GENERATOR  "${CMAKE_GENERATOR}"
        CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${_CU_WASM_TOOLCHAIN}
            -DCU_WASM_ALGO=${ALGO}
            # Default wasm to Release even when the parent is Debug — DWARF
            # in .wasm balloons artifact size 6x and isn't useful in a
            # browser context. Override with -DCU_WASM_BUILD_TYPE=Debug.
            -DCMAKE_BUILD_TYPE=${CU_WASM_BUILD_TYPE}
    )
    add_dependencies(compress_utils_wasm ${_PROJ})
endforeach()
