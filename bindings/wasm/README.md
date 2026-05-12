# compress-utils — WASM bindings

Per-algorithm WebAssembly modules for `compress-utils`. Each algorithm
ships as a separate `.wasm` so bundlers can tree-shake on subpath import:

```ts
import { compress, decompress } from "compress-utils/zstd";

const compressed = await compress(new TextEncoder().encode("hello"));
const roundtrip = await decompress(compressed);
```

If you import `compress-utils/zstd`, only `zstd.wasm` (~150 KB) lands in
your bundle. Importing `compress-utils/brotli` pulls in `brotli.wasm`
independently. There is no combined module.

## Status

**Scaffolded, not yet verified end-to-end.** As of 2026-05-11:

- Zig→WASM CMake toolchain landed (`cmake/toolchains/zig-wasm.cmake`).
- Per-algorithm CMake project landed (`bindings/wasm/CMakeLists.txt`).
- TypeScript dispatcher + zstd subpath landed.
- A full `npm run build && npm test` round-trip has **not** been
  executed yet — pending a build host with `zig` (>=0.13) and `node`
  (>=18) installed.

The other five algorithms (`brotli`, `zlib`, `bz2`, `lz4`, `xz`) will be
added once zstd is proven end-to-end. Each is one ~15-line `index.ts`
plus invoking the existing `scripts/build-all.sh`.

## Build

From this directory:

```bash
npm run build      # configures root CMake with -DBUILD_WASM_BINDINGS=ON,
                   # then builds the compress_utils_wasm target, then tsc.
npm test           # vitest round-trip
```

Or invoke CMake yourself from the repo root:

```bash
cmake -S . -B build -DBUILD_WASM_BINDINGS=ON
cmake --build build --target compress_utils_wasm
```

`BUILD_WASM_BINDINGS` defaults to OFF so native builds aren't slowed by the
wasm cross-compile. Limit to specific algorithms with `-DCU_WASM_ALGOS="zstd;brotli"`.

Prerequisites: `zig` ≥ 0.13 on PATH (`zig cc` is the cross-compiler to
`wasm32-wasi`), CMake ≥ 3.17, Node ≥ 18. `wasm-strip` + `wasm-opt` (from
WABT + binaryen) are auto-detected and used to shrink artifacts; if
absent, the build still succeeds but `.wasm` files stay 3–4× larger.

## Layout

```
bindings/wasm/
  CMakeLists.txt              Per-algorithm WASM build.
  scripts/build-all.sh        Driver: invokes CMake 6 times.
  src/
    core/types.ts             Enums + CompressError. Mirrors compress_utils.h.
    core/loader.ts            WebAssembly.instantiate + WASI reactor shim.
    core/dispatch.ts          JS↔C ABI marshalling, drain protocol, streams.
    algorithms/<algo>/
      index.ts                ~50 LOC public surface, lazy-loads <algo>.wasm.
      <algo>.wasm             Built by CMake.
  tests/                      Per-algorithm round-trip tests.
  dist/                       npm publish artifact.
```

The shared `dispatch.ts` (~250 lines) is the only non-trivial JS in the
package. Each algorithm's `index.ts` is mechanical — bind the dispatcher
to its `.wasm` factory and re-export.

## How the build works

1. `scripts/build-all.sh` calls CMake six times, once per algorithm, each
   in its own `build-<algo>/` directory.
2. Each invocation configures `bindings/wasm/CMakeLists.txt` with the
   `cmake/toolchains/zig-wasm.cmake` toolchain and
   `-DCU_WASM_ALGO=<algo>`.
3. The toolchain points `CMAKE_C_COMPILER` at `zig cc` (via the wrappers
   in `cmake/toolchains/zig-bin/`) and sets `-target wasm32-wasi
   -mexec-model=reactor`.
4. `bindings/wasm/CMakeLists.txt` reuses the existing
   `algorithms/<algo>/CMakeLists.txt` subproject — its
   `ExternalProject_Add` inherits the toolchain automatically, so
   upstream zstd/brotli/etc. get built for wasm32-wasi alongside our
   wrapper.
5. Output staged at `dist/algorithms/<algo>/<algo>.wasm`.

## Memory model

The JS dispatcher allocates wasm linear memory via `cu_alloc`/`cu_free`
(exported from `src/wasm_runtime.c`), copies caller bytes in, calls the
C ABI, copies bytes out, and frees. Linear memory may grow during a
call (codec allocations), so we re-read `exports.memory.buffer` on
every operation — Uint8Array views are not held across calls.

## Why Zig and not Emscripten

The TODO has the rationale ("Zig→WASM ergonomics, single toolchain for
WASM + aarch64 cross"). In short: one `zig` install replaces `emsdk`
version-pinning, and the same toolchain pattern unlocks the aarch64
Linux CI target.
