# Zig→WASM cross-compile toolchain.
#
# Usage:
#   cmake -S bindings/wasm -B build-wasm \
#         --toolchain cmake/toolchains/zig-wasm.cmake \
#         -DCU_WASM_ALGO=zstd
#
# Targets `wasm32-wasi` in reactor mode — gives us libc (malloc/free/memcpy
# the upstream codecs need) without an entry point. JS host instantiates
# the module with a tiny WASI polyfill (see bindings/wasm/src/core/loader.ts).
#
# Wrapper scripts in cmake/toolchains/zig-bin/ exist because CMake's
# CMAKE_C_COMPILER cannot hold a multi-word command ("zig cc"). The wrappers
# are 1-line `exec zig cc "$@"` shims.

set(CMAKE_SYSTEM_NAME       WASI)
set(CMAKE_SYSTEM_PROCESSOR  wasm32)
set(CMAKE_CROSSCOMPILING    ON)

set(_CU_ZIG_BIN ${CMAKE_CURRENT_LIST_DIR}/zig-bin)

set(CMAKE_C_COMPILER   ${_CU_ZIG_BIN}/zig-cc)
set(CMAKE_CXX_COMPILER ${_CU_ZIG_BIN}/zig-c++)
set(CMAKE_AR           ${_CU_ZIG_BIN}/zig-ar)
set(CMAKE_RANLIB       ${_CU_ZIG_BIN}/zig-ranlib)

# Target every C/C++ invocation at wasm32-wasi.
set(CU_WASM_TARGET "wasm32-wasi")
set(CMAKE_C_FLAGS_INIT          "-target ${CU_WASM_TARGET}")
set(CMAKE_CXX_FLAGS_INIT        "-target ${CU_WASM_TARGET}")

# Reactor model: module exports its API but doesn't expect a `main`. The
# linker still synthesizes and exports `_initialize` (libc ctors) and the
# `memory` in this mode, which is all the JS loader needs beyond our ABI.
#
# We deliberately do NOT pass `-Wl,--export-dynamic`. That flag roots every
# default-visibility symbol as a wasm export, and the upstream codec archives
# (libzstd.a, brotli, …) are built in their own CMake subprojects that don't
# inherit our hidden-visibility preset — so their entire public API ends up
# rooted, defeating wasm-ld GC and wasm-opt DCE. The explicit `--export=`
# allow-list of the cu_* ABI lives in bindings/wasm/CMakeLists.txt instead.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-target ${CU_WASM_TARGET} -mexec-model=reactor")

# WASI is a bare runtime — TRY_COMPILE can't link an executable that runs
# anywhere, so use a static lib for the probe.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Don't search the host system for headers/libs; everything we link must
# come from the project or from `zig cc`'s bundled sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Marker the wasm CMakeLists checks to refuse misconfiguration.
set(CU_WASM_TOOLCHAIN ON)
