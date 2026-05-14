# compress-utils WASM — development notes

Internal doc for contributors. Not published to npm (the user-facing `README.md` is).

## Status

All six algorithms (`zstd`, `brotli`, `zlib`, `bz2`, `lz4`, `xz`) build
end-to-end. CI runs the runtime matrix (Node 20 + 22, Bun, Deno) plus
Playwright (Chromium + Firefox + WebKit) on every PR that touches WASM
paths, every push to `main`, and every `v*` tag.

Per-algo artifact sizes (post `wasm-strip` + `wasm-opt -O3`):

| algo | size |
|---|---|
| zlib | 80 KB |
| bz2 | 95 KB |
| xz | 135 KB |
| lz4 | 140 KB |
| zstd | 545 KB |
| brotli | 730 KB |

Outstanding TODOs (see repo-root `TODO.md` for the canonical list):
- Drop zstd / brotli sizes further (`-Oz` per-algo, strip legacy decoders).
- Decide whether to migrate the WASM build off CMake onto `build.zig`
  (currently deferred — not worth the migration cost while CI is green).

## Build

From this directory:

```bash
npm run build      # cmake configure → build compress_utils_wasm target → tsc
npm test           # vitest (33 tests across all six algos)
```

Or directly:

```bash
cmake -S ../.. -B ../../build-wasm -DBUILD_WASM_BINDINGS=ON
cmake --build ../../build-wasm --target compress_utils_wasm
```

`BUILD_WASM_BINDINGS` defaults to OFF in the root project so native
builds aren't slowed by the wasm cross-compile. Filter algorithms with
`-DCU_WASM_ALGOS="zstd;brotli"` (defaults to whatever the parent
`INCLUDE_<ALGO>` flags enable — see `host.cmake`).

Prerequisites: `zig` ≥ 0.13 (`zig cc` is the cross-compiler to
`wasm32-wasi`), CMake ≥ 3.17, Node ≥ 18. `wasm-strip` + `wasm-opt`
(WABT + binaryen) are auto-detected and used to shrink artifacts. If
they're absent the build still succeeds, but `.wasm` files stay 3–4×
larger.

## Layout

```
bindings/wasm/
  CMakeLists.txt              Per-algorithm WASM build (separate CMake project).
  host.cmake                  Driver for the root CMakeLists: one ExternalProject
                              per algo, configured with the zig-wasm toolchain.
  src/
    core/types.ts             AlgorithmName + CompressError + options.
    core/dispatch.ts          JS↔C ABI marshalling, drain protocol, streams,
                              createBindings factory.
    core/loader.ts            WebAssembly.instantiate + WASI reactor shim.
    core/resolve-browser.ts   fetch-based resolver (streaming compile).
    core/resolve-node.ts      fs.readFile-based resolver.
    core/resolve-default.ts   runtime-detect fallback.
    algorithms/<algo>/
      index.ts                Default entry: runtime-detect.
      index.browser.ts        Browser / Deno / Bun: fetch only, no node imports.
      index.node.ts           Node: static fs import.
      <algo>.wasm             Built by CMake.
  tests/                      Vitest suites (Node-side).
  tests-browser/              Playwright spec + static-server harness.
  tests-runtime/              Deno smoke test.
  dist/                       npm publish artifact (everything below dist/ ships).
```

The shared `dispatch.ts` (~400 LOC) is the only non-trivial JS in the
package. Per-algo entries are ~25 LOC each — they bind the dispatcher
factory to one algorithm + one `.wasm` URL + one resolver.

## How the build works

1. `CMakeLists.txt` (top-level) includes `bindings/wasm/host.cmake` when
   `BUILD_WASM_BINDINGS=ON`.
2. `host.cmake` does an `ExternalProject_Add` per algorithm. Each child
   re-runs CMake with the `cmake/toolchains/zig-wasm.cmake` toolchain
   and `-DCU_WASM_ALGO=<algo>`.
3. The toolchain points `CMAKE_C_COMPILER` at `zig cc` via the wrappers
   in `cmake/toolchains/zig-bin/`, and sets `-target wasm32-wasi
   -mexec-model=reactor`.
4. `bindings/wasm/CMakeLists.txt` reuses the existing
   `algorithms/<algo>/CMakeLists.txt` subproject. Its
   `ExternalProject_Add` inherits `CMAKE_TOOLCHAIN_FILE` automatically,
   so upstream codecs compile for `wasm32-wasi` alongside our wrapper.
5. POST_BUILD runs `wasm-strip` then `wasm-opt -O3
   --enable-bulk-memory --enable-sign-ext
   --enable-nontrapping-float-to-int --enable-mutable-globals`. Output
   staged at `dist/algorithms/<algo>/<algo>.wasm`.

## Memory model

The dispatcher allocates wasm linear memory via `cu_alloc`/`cu_free`
(exported from `src/wasm_runtime.c`), copies caller bytes in, calls the
C ABI, copies bytes out, and frees. Linear memory may grow during a
call (codec internal allocations) — so we re-read
`exports.memory.buffer` on every operation, and never hold a
`Uint8Array` view across calls.

## Per-codec build quirks

These are the fixes we paid for during the initial six-algo bring-up.
Reference for the day we touch any of them again.

- **brotli** — `BUILD_COMMAND` overridden to skip the `brotli` CLI
  target (uses `chown` / `setresuid`, not in WASI). Builds only
  `brotlienc` / `brotlidec` / `brotlicommon`.
- **xz** — `ENABLE_THREADS=OFF` so liblzma doesn't try to compile its
  pthread / signal-mask threaded encoder.
- **zlib** — wasm cross-compile produces `libzlibstatic.a` rather than
  the renamed `libz.a` (the rename is inside `if(UNIX)`, which excludes
  WASI). We sniff the toolchain path and pick the right name.
- **bz2** — runtime loader returns `EBADF` (not `ENOSYS`) from
  `fd_prestat_get` so wasi-libc's preopen scan terminates cleanly
  during `_initialize`.
- **zstd** — install step copies `zstd_errors.h` alongside `zstd.h`
  (v1.5.7 needs it).

## Why Zig (and not Emscripten)

One `zig` install replaces emsdk version-pinning, and the same toolchain
pattern unlocks the aarch64 Linux CI target. See repo-root `TODO.md`
under "Phase 4 — Reshape bindings" for the original decision.
