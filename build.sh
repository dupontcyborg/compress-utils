#!/usr/bin/env bash
#
# build.sh — top-level driver that quarterbacks the right build system
# for each language binding.
#
#   C / C++ / Python / WASM → CMake (cmake/CMakeLists.txt + bindings/wasm)
#   Zig                     → `zig build` in bindings/zig (when present)
#
# Single source of truth for upstream codec versions lives in
# codec-versions.json. This script runs tools/sync-codecs.py at the
# start so any drift between the manifest and bindings/zig/build.zig.zon
# is repaired (or reported) before either build system runs.

set -euo pipefail

BUILD_DIR="build"
CLEAN_BUILD=false
SKIP_TESTS=false
SKIP_SYNC=false
REVENDOR=false
BUILD_MODE="Release"
CORES=1
ALGORITHMS=()
LANGUAGES=()

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Options:
  --clean                    Clean every build directory + dist/ before building.
  --skip-tests               Skip ctest / language-binding test suites.
  --skip-sync                Skip the codec-version sync step (use with care).
  --revendor                 Regenerate third_party/ from codec-versions.json
                             (downloads the pinned tags) before building.
  --debug                    Build in Debug instead of Release.
  --algorithms=LIST          Comma-separated list. Default: all.
                             Available: brotli, bz2 (bzip2), lz4, zstd, zlib, xz (lzma)
  --languages=LIST           Comma-separated list. Default: c, cpp, python.
                             Available: c, cpp (c++), python, wasm, zig, go
  --cores=N                  Parallel build cores. Default: 1.
  -h, --help                 Show this help.

Examples:
  $0 --clean --algorithms=zstd,zlib --languages=python
  $0 --languages=wasm
  $0 --languages=zig
  $0 --languages=c,cpp,python,wasm,zig    # the works
EOF
    exit "${1:-1}"
}

# ---------- parse args ---------------------------------------------------------

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --clean)        CLEAN_BUILD=true ;;
        --skip-tests)   SKIP_TESTS=true ;;
        --skip-sync)    SKIP_SYNC=true ;;
        --revendor)     REVENDOR=true ;;
        --debug)        BUILD_MODE="Debug" ;;
        --algorithms=*) IFS=',' read -ra ALGORITHMS <<< "${1#*=}" ;;
        --languages=*)  IFS=',' read -ra LANGUAGES  <<< "${1#*=}" ;;
        --cores=*)      CORES="${1#*=}" ;;
        -h|--help)      usage 0 ;;
        *)              echo "Unknown option: $1" >&2; usage 1 ;;
    esac
    shift
done

# Default language set if user didn't specify one.
if [[ ${#LANGUAGES[@]} -eq 0 ]]; then
    LANGUAGES=(c cpp python)
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

# ---------- categorize requested languages -------------------------------------

WANT_C=false
WANT_CPP=false
WANT_PYTHON=false
WANT_WASM=false
WANT_ZIG=false
WANT_GO=false

for lang in "${LANGUAGES[@]}"; do
    case "$lang" in
        c)              WANT_C=true ;;
        cpp|c++)        WANT_CPP=true ;;
        python|py)      WANT_PYTHON=true ;;
        wasm|js|ts)     WANT_WASM=true ;;
        zig)            WANT_ZIG=true ;;
        go|golang)      WANT_GO=true ;;
        *)              echo "Unknown language: $lang" >&2; usage 1 ;;
    esac
done

# C ABI is always built (it's the library's core). Any other binding pulls it in
# transitively. C++/Python/WASM are CMake-built; Zig has its own build.zig.
NEEDS_CMAKE=false
if $WANT_C || $WANT_CPP || $WANT_PYTHON || $WANT_WASM; then
    NEEDS_CMAKE=true
fi

# ---------- pre-flight: codec sync --------------------------------------------

if ! $SKIP_SYNC; then
    if [[ -f tools/sync-codecs.py ]] && command -v python3 >/dev/null 2>&1; then
        # If the Zig binding isn't there yet, the script no-ops. Otherwise it
        # regenerates build.zig.zon. Always cheap.
        echo ">>> Syncing codec versions from codec-versions.json"
        python3 tools/sync-codecs.py
    else
        echo "note: skipping codec sync (tools/sync-codecs.py or python3 missing)" >&2
    fi
fi

# ---------- revendor ----------------------------------------------------------
# Optional: regenerate third_party/ from the pinned tags in codec-versions.json.
# Not part of a normal build — the vendored tree is committed and consumed
# directly. Use after bumping a codec version.

if $REVENDOR; then
    if [[ -f tools/vendor-codecs.py ]] && command -v python3 >/dev/null 2>&1; then
        echo ">>> Re-vendoring upstream codec sources into third_party/"
        python3 tools/vendor-codecs.py
    else
        echo "error: --revendor needs tools/vendor-codecs.py and python3" >&2
        exit 1
    fi
fi

# ---------- clean -------------------------------------------------------------

if $CLEAN_BUILD; then
    echo ">>> Cleaning build directories"
    rm -rf "$BUILD_DIR" dist algorithms/dist
    rm -rf algorithms/*/build
    rm -rf bindings/*/build bindings/*/dist
    # Zig's local caches (only present if Zig binding has been built).
    rm -rf bindings/zig/zig-cache bindings/zig/.zig-cache
fi

# ---------- CMake path: C / C++ / Python / WASM -------------------------------

if $NEEDS_CMAKE; then
    echo ""
    echo ">>> CMake build path (C / C++ / Python / WASM)"

    if ! command -v cmake >/dev/null 2>&1; then
        echo "error: cmake required for the C / C++ / Python / WASM bindings" >&2
        exit 1
    fi

    CMAKE_OPTS=( -DCMAKE_BUILD_TYPE="$BUILD_MODE" )

    # Algorithm gating.
    if [[ ${#ALGORITHMS[@]} -gt 0 ]]; then
        # Start with everything off, enable per request.
        CMAKE_OPTS+=( -DINCLUDE_BROTLI=OFF -DINCLUDE_BZ2=OFF -DINCLUDE_LZ4=OFF
                      -DINCLUDE_XZ=OFF    -DINCLUDE_ZSTD=OFF -DINCLUDE_ZLIB=OFF )
        for algo in "${ALGORITHMS[@]}"; do
            case "$algo" in
                brotli)     CMAKE_OPTS+=( -DINCLUDE_BROTLI=ON ) ;;
                bz2|bzip2)  CMAKE_OPTS+=( -DINCLUDE_BZ2=ON ) ;;
                lz4)        CMAKE_OPTS+=( -DINCLUDE_LZ4=ON ) ;;
                xz|lzma)    CMAKE_OPTS+=( -DINCLUDE_XZ=ON ) ;;
                zstd)       CMAKE_OPTS+=( -DINCLUDE_ZSTD=ON ) ;;
                zlib)       CMAKE_OPTS+=( -DINCLUDE_ZLIB=ON ) ;;
                *)          echo "Unknown algorithm: $algo" >&2; usage 1 ;;
            esac
        done
    fi
    # Default is all-on per the CMakeLists, no flag needed.

    # Binding selection.
    CMAKE_OPTS+=(
        -DBUILD_CPP_BINDINGS=$( $WANT_CPP && echo ON || echo OFF )
        -DBUILD_PYTHON_BINDINGS=$( $WANT_PYTHON && echo ON || echo OFF )
        -DBUILD_WASM_BINDINGS=$( $WANT_WASM && echo ON || echo OFF )
    )
    if $WANT_PYTHON; then
        CMAKE_OPTS+=( -DPython3_EXECUTABLE="$(command -v python3)" )
    fi
    if $SKIP_TESTS; then
        CMAKE_OPTS+=( -DENABLE_TESTS=OFF )
    fi

    mkdir -p "$BUILD_DIR"
    echo "    cmake $(printf '%q ' "${CMAKE_OPTS[@]}")"
    cmake -S . -B "$BUILD_DIR" "${CMAKE_OPTS[@]}"
    cmake --build "$BUILD_DIR" -j "$CORES"

    # WASM is its own target — only build it when requested. (Default
    # `cmake --build` doesn't include compress_utils_wasm because the
    # ExternalProject for each algo is opt-in via the root target.)
    if $WANT_WASM; then
        cmake --build "$BUILD_DIR" --target compress_utils_wasm -j "$CORES"
    fi

    cmake --install "$BUILD_DIR"

    if ! $SKIP_TESTS; then
        echo ">>> Running ctest"
        ctest --test-dir "$BUILD_DIR" --output-on-failure -C "$BUILD_MODE"
    fi
fi

# ---------- Zig path ----------------------------------------------------------

if $WANT_ZIG; then
    echo ""
    echo ">>> Zig build path"

    if ! command -v zig >/dev/null 2>&1; then
        echo "error: 'zig' not found on PATH (need >= 0.13 for the Zig binding)" >&2
        exit 1
    fi

    if [[ ! -f bindings/zig/build.zig ]]; then
        cat >&2 <<EOF
error: bindings/zig/build.zig does not exist yet.

The Zig binding hasn't been written. The codec-versions.json manifest +
tools/sync-codecs.py + build.sh quarterbacking are all wired up to support
it, but the binding source itself is TODO. See TODO.md under "Bindings".
EOF
        exit 1
    fi

    ZIG_OPTIMIZE="ReleaseFast"
    if [[ "$BUILD_MODE" == "Debug" ]]; then
        ZIG_OPTIMIZE="Debug"
    fi

    pushd bindings/zig >/dev/null
    zig build -Doptimize="$ZIG_OPTIMIZE" -j"$CORES"
    if ! $SKIP_TESTS; then
        zig build test -Doptimize="$ZIG_OPTIMIZE"
    fi
    popd >/dev/null
fi

# ---------- Go path -----------------------------------------------------------
# The Go binding compiles the vendored third_party/ sources via cgo — no CMake,
# no prebuilt lib. The go.mod is at the repo root (so third_party is in-module),
# and the generated cgo shims under bindings/go/ are committed. We only verify
# they match the manifest here; `go build` compiles everything from source.

if $WANT_GO; then
    echo ""
    echo ">>> Go build path"

    if ! command -v go >/dev/null 2>&1; then
        echo "error: 'go' not found on PATH (need >= 1.21 for the Go binding)" >&2
        exit 1
    fi

    if command -v python3 >/dev/null 2>&1; then
        echo ">>> Checking generated cgo shims are in sync with the manifest"
        python3 tools/gen-go-cgo.py --check
    fi

    CGO_ENABLED=1 go build ./bindings/go/
    if ! $SKIP_TESTS; then
        echo ">>> Running go test"
        CGO_ENABLED=1 go test ./bindings/go/
    fi
fi

# ---------- size summary ------------------------------------------------------

if $NEEDS_CMAKE && [[ -d dist ]]; then
    echo ""
    echo "Sizes of built libraries:"
    echo "-------------------------"
    find dist -type f \( -name "*compress_utils*" -o -name "*.wasm" \) \
        -exec du -sh {} + 2>/dev/null \
        | awk '{print $2 ": " $1}'
fi
