# Development

Building compress-utils from source, and the test suites.

## Prerequisites

- CMake 3.17+
- A C11 compiler (Clang, GCC, MSVC)
- A C++20 compiler (only for the C++ binding and the pybind11 module)
- Python 3.11+ and `pybind11-stubgen` (only for the Python binding)

## Building

```sh
git clone https://github.com/dupontcyborg/compress-utils.git
cd compress-utils
./build.sh                  # Linux / macOS
# or:
powershell -File build.ps1  # Windows
```

The default build produces:

- `dist/c/lib/libcompress_utils.{dylib,so,dll}` — the shared C library, self-contained (all eight algorithms baked in).
- `dist/c/include/compress_utils.h` — the public C header.
- `dist/cpp/include/compress_utils.hpp` — the header-only C++ binding.
- `bindings/python/compress_utils/` — the importable Python package, including auto-generated `.pyi` type stubs.

Builds default to Release (LTO, `-O3` / `/O2`). Useful flags:

- `--algorithms=zstd,zlib` — limit which compressors are included (smaller binary). Default: all.
- `--languages=c,cpp,python,wasm,zig` — which bindings to build. Default: `c,cpp,python` (C is the core and is always built).
- `--debug` — Debug build instead of the default Release.
- `--cores=N` — parallel build cores (default: 1).
- `--clean` — clean every build directory + `dist/` before building.
- `--skip-tests` — don't build/run the test suites.

For raw CMake usage (without `build.sh`):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

## Testing

Each binding has its own test suite, all wired through ctest:

| Target | What it covers |
|--------|----------------|
| `test_compress_utils` (C) | One-shot, streaming with tight buffers, cross-API round-trip, error codes, edge cases |
| `test_compress_utils_cpp` (C++) | `cu::` namespace surface, RAII semantics, exception translation |
| `test_compress_utils_py` (Python) | Same surface via pybind11, plus 1MB random/repetitive cases, string-vs-enum spellings |
| `test_snappy_oracle` (C++) | The `cu_*` API's Snappy output and input against the reference google/snappy (`third_party/snappy-oracle`, test-only) |
| `test_snappy_core_diff` (C++) | Our Snappy core (`src/algorithms/snappy/core`) linked directly against the same oracle: both directions on many inputs, and on corrupted, truncated and generated inputs the same accept/reject and the same bytes. Buffers sit against guard pages so any out-of-bounds access faults without a sanitizer. `SNAPPY_DIFF_ITERS=2000000` runs it harder |

Plus a libFuzzer harness at `tests/fuzz/fuzz_decompress.c` (see [Fuzzing](#fuzzing)); every build also runs its saved regression inputs as the `fuzz_regressions` ctest.

## Fuzzing

`tests/fuzz/fuzz_decompress.c` feeds mutated input to every decoder: the one-shot path, the size hint, and the streaming decoder with varied chunk and buffer sizes, in default and single-stream mode. It fails on crashes, leaks, ASan/UBSan reports, decoders that stall in a drain loop, and single-stream contract violations. The first input byte picks the codec (an index into `tests/fuzz/fuzz_algos.h`, append only) and the second picks the streaming mode.

A second harness, `tests/fuzz/fuzz_roundtrip.c`, fuzzes the encoders: the input bytes are the data, which must compress (one-shot into exactly `cu_compress_bound` bytes, and through the streaming encoder with small buffers and flushes) and decompress back to themselves. Saved inputs for it live in `tests/fuzz/regressions-roundtrip/`, replayed by the `fuzz_roundtrip_regressions` ctest.

CI (`.github/workflows/fuzz.yml`) fuzzes the decoders for 3 minutes on every PR and 30 minutes nightly on Linux, keeping the corpus the nightly run grows in the Actions cache. Crash inputs are uploaded as the `fuzz-artifacts` artifact.

To fuzz locally you need a clang that ships libFuzzer. Apple's clang doesn't, so on macOS use Homebrew's `llvm`. `-DENABLE_FUZZ=ON` instruments every target, including the vendored codecs, for coverage and ASan/UBSan:

```sh
CC=clang CXX=clang++ cmake -B build-fuzz -DENABLE_FUZZ=ON -DCMAKE_BUILD_TYPE=Debug -DBUILD_PYTHON_BINDINGS=OFF
cmake --build build-fuzz -j --target fuzz_decompress fuzz_seeds
mkdir -p build-fuzz/corpus
./build-fuzz/tests/fuzz/fuzz_decompress build-fuzz/corpus build-fuzz/tests/fuzz/seeds -max_total_time=300 -timeout=10
```

`fuzz_seeds` generates the seed corpus (valid frames per codec, made by the library itself) into `build-fuzz/tests/fuzz/seeds`. New corpus entries go to the first directory. Add `-fork=N` to use N cores.

On macOS 27, ASan from Homebrew LLVM (18 and 21) hangs at startup, before `main`. Either configure with `-DCU_FUZZ_SANITIZERS=undefined` (fuzzing without memory-error detection) or fuzz in a Linux container, as CI does.

When the fuzzer finds something it writes `crash-*`, `timeout-*`, `oom-*` or `leak-*` files. To reproduce one, run it alone: `./build-fuzz/tests/fuzz/fuzz_decompress crash-<hash>`. Once it's fixed, copy the input into `tests/fuzz/regressions/` under a descriptive name and add a row to the README there, so `fuzz_regressions` replays it on every build from then on.

## Coverage

Each part of the library is measured only by the tests written in its own language, so one suite can't make another look better covered than it is: the C core (`src/`) by the C tests, and each binding by its own suite. CI (`.github/workflows/coverage.yml`) reports all of them on every PR, in each job's summary plus an HTML artifact, and fails a job whose part is below 90% line coverage or 95% function coverage (`COVERAGE_MIN_LINES` and `COVERAGE_MIN_FUNCTIONS` in the workflow; statements stand in for lines in Go, which has no line metric). The nightly fuzz run also reports which code its corpus reaches, including the vendored decoders.

| Part | Measured by | Run locally |
|------|-------------|-------------|
| C core, C++ binding, Python binding (pybind11 glue) | the C, C++ and Python tests | `tools/coverage.py` on a coverage build (below) |
| Go binding | `go test` | `go test -coverprofile=go-coverage.out ./bindings/go/ && go tool cover -func=go-coverage.out` |
| Rust binding | `cargo test` | `cargo llvm-cov` (install with `cargo install cargo-llvm-cov`) |
| WASM binding (TypeScript) | vitest | `npm run build && npx vitest run --coverage` in `bindings/wasm` |

For the C-family parts, configure with clang's source-based coverage, then run the script, which runs each language's tests separately and writes reports to `coverage/` (`coverage/core-html/index.html` and so on). On macOS, Apple's clang works; the script finds `llvm-cov` through `xcrun`.

```sh
F="-fprofile-instr-generate -fcoverage-mapping"
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_C_FLAGS=$F" "-DCMAKE_CXX_FLAGS=$F" -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate -DCMAKE_SHARED_LINKER_FLAGS=-fprofile-instr-generate -DCMAKE_MODULE_LINKER_FLAGS=-fprofile-instr-generate
cmake --build build-cov -j
tools/coverage.py --build-dir build-cov
```

Pass `--groups core` (or `cpp`, `python`) for one part, and `--min-lines 90 --min-functions 95` to apply CI's minimums. Building the Python binding writes its module into `bindings/python/compress_utils/`, replacing one you built normally.

## Adding an algorithm

See [docs/adding-an-algorithm.md](docs/adding-an-algorithm.md) for the end-to-end walkthrough of wiring a new codec through the C core and every binding.
