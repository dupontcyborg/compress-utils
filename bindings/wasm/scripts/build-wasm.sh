#!/usr/bin/env bash
#
# Build WASM modules for compress-utils.
#
# This script compiles the C++ compression algorithms to WebAssembly using Emscripten.
# Each algorithm is built as a separate WASM module for tree-shakeability.
#
# Prerequisites:
#   - Emscripten SDK installed and activated (emcc in PATH)
#
# Usage:
#   ./scripts/build-wasm.sh [options]
#
# Options:
#   --algorithms=LIST  Comma-separated list of algorithms to build (default: all)
#   --debug            Build with debug symbols
#   --clean            Clean build artifacts before building
#   --help             Show this help message
#

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WASM_DIR="$(dirname "$SCRIPT_DIR")"
PROJECT_ROOT="$(dirname "$(dirname "$WASM_DIR")")"

# Output directories
BUILD_DIR="${WASM_DIR}/wasm-build"
GENERATED_DIR="${WASM_DIR}/src/algorithms"

# Directory for downloaded algorithm sources
DEPS_DIR="${WASM_DIR}/deps"

# All supported algorithms
ALL_ALGORITHMS="zstd brotli zlib bz2 lz4 xz"

# Default options
ALGORITHMS="$ALL_ALGORITHMS"
DEBUG=false
CLEAN=false

# Parse arguments
for arg in "$@"; do
  case $arg in
    --algorithms=*)
      ALGORITHMS="${arg#*=}"
      ALGORITHMS="${ALGORITHMS//,/ }"
      ;;
    --debug)
      DEBUG=true
      ;;
    --clean)
      CLEAN=true
      ;;
    --help)
      head -30 "$0" | tail -26
      exit 0
      ;;
    *)
      echo -e "${RED}Unknown option: $arg${NC}"
      exit 1
      ;;
  esac
done

# Verify Emscripten is available
if ! command -v emcc &> /dev/null; then
  echo -e "${RED}Error: emcc not found. Please install and activate the Emscripten SDK.${NC}"
  echo "Visit: https://emscripten.org/docs/getting_started/downloads.html"
  exit 1
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║        compress-utils WASM Build                          ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "Emscripten version: $(emcc --version | head -1)"
echo -e "Algorithms: ${ALGORITHMS}"
echo -e "Debug mode: ${DEBUG}"
echo ""

# Clean if requested
if [ "$CLEAN" = true ]; then
  echo -e "${YELLOW}Cleaning build artifacts...${NC}"
  rm -rf "$BUILD_DIR"
  rm -rf "$DEPS_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"
mkdir -p "$DEPS_DIR"

# Function to download algorithm sources if needed
download_algorithm_sources() {
  echo -e "${BLUE}Checking algorithm sources...${NC}"

  # zstd
  if [[ " $ALGORITHMS " =~ " zstd " ]] && [ ! -d "${DEPS_DIR}/zstd" ]; then
    echo -e "  Downloading zstd..."
    git clone --depth 1 --branch v1.5.6 https://github.com/facebook/zstd.git "${DEPS_DIR}/zstd" 2>/dev/null || true
  fi

  # brotli
  if [[ " $ALGORITHMS " =~ " brotli " ]] && [ ! -d "${DEPS_DIR}/brotli" ]; then
    echo -e "  Downloading brotli..."
    git clone --depth 1 --branch v1.1.0 https://github.com/google/brotli.git "${DEPS_DIR}/brotli" 2>/dev/null || true
  fi

  # zlib
  if [[ " $ALGORITHMS " =~ " zlib " ]] && [ ! -d "${DEPS_DIR}/zlib" ]; then
    echo -e "  Downloading zlib..."
    git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git "${DEPS_DIR}/zlib" 2>/dev/null || true
  fi

  # bz2
  if [[ " $ALGORITHMS " =~ " bz2 " ]] && [ ! -d "${DEPS_DIR}/bz2" ]; then
    echo -e "  Downloading bzip2..."
    git clone --depth 1 --branch bzip2-1.0.8 https://gitlab.com/bzip2/bzip2.git "${DEPS_DIR}/bz2" 2>/dev/null || true
  fi

  # lz4
  if [[ " $ALGORITHMS " =~ " lz4 " ]] && [ ! -d "${DEPS_DIR}/lz4" ]; then
    echo -e "  Downloading lz4..."
    git clone --depth 1 --branch v1.10.0 https://github.com/lz4/lz4.git "${DEPS_DIR}/lz4" 2>/dev/null || true
  fi

  # xz (liblzma)
  if [[ " $ALGORITHMS " =~ " xz " ]] && [ ! -d "${DEPS_DIR}/xz" ]; then
    echo -e "  Downloading xz..."
    git clone --depth 1 --branch v5.6.3 https://github.com/tukaani-project/xz.git "${DEPS_DIR}/xz" 2>/dev/null || true
  fi

  echo -e "${GREEN}  Algorithm sources ready${NC}"
  echo ""
}

# Download algorithm sources
download_algorithm_sources

# Common Emscripten flags
COMMON_FLAGS=(
  "-s" "WASM=1"
  "-s" "MODULARIZE=1"
  "-s" "EXPORT_ES6=1"
  "-s" "ALLOW_MEMORY_GROWTH=1"
  "-s" "MALLOC=emmalloc"
  "-s" "FILESYSTEM=0"
  "-s" "ENVIRONMENT=web,node"
  "-s" "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','wasmMemory']"
  "-s" "SINGLE_FILE=1"
  "--no-entry"
)

if [ "$DEBUG" = true ]; then
  COMMON_FLAGS+=("-g" "-O0" "-s" "ASSERTIONS=2")
else
  COMMON_FLAGS+=("-Oz" "-flto" "--closure" "1")
fi

# Function to build a single algorithm
build_algorithm() {
  local algo=$1
  local algo_upper="${algo^^}"
  local binding_cpp="${GENERATED_DIR}/${algo}/binding.cpp"
  local output_wasm="${BUILD_DIR}/${algo}.wasm"
  local output_js="${BUILD_DIR}/${algo}.js"

  echo -e "${BLUE}Building ${algo}...${NC}"

  # Check if binding file exists
  if [ ! -f "$binding_cpp" ]; then
    echo -e "${YELLOW}  Skipping ${algo}: binding.cpp not found${NC}"
    return 0
  fi

  # Algorithm-specific source files and include paths
  local algo_sources=()
  local algo_includes=()
  local algo_libs=()

  case $algo in
    zstd)
      algo_includes=(
        "-I${DEPS_DIR}/zstd/lib"
        "-I${DEPS_DIR}/zstd/lib/common"
      )
      # Collect zstd source files (glob expansion happens here)
      algo_sources=()
      for f in "${DEPS_DIR}"/zstd/lib/common/*.c "${DEPS_DIR}"/zstd/lib/compress/*.c "${DEPS_DIR}"/zstd/lib/decompress/*.c; do
        [ -f "$f" ] && algo_sources+=("$f")
      done
      ;;
    brotli)
      algo_includes=(
        "-I${DEPS_DIR}/brotli/c/include"
      )
      algo_sources=()
      for f in "${DEPS_DIR}"/brotli/c/common/*.c "${DEPS_DIR}"/brotli/c/enc/*.c "${DEPS_DIR}"/brotli/c/dec/*.c; do
        [ -f "$f" ] && algo_sources+=("$f")
      done
      ;;
    zlib)
      algo_includes=(
        "-I${DEPS_DIR}/zlib"
      )
      # zlib source files (excluding gz* files that require filesystem, and test/example files)
      algo_sources=(
        "${DEPS_DIR}/zlib/adler32.c"
        "${DEPS_DIR}/zlib/compress.c"
        "${DEPS_DIR}/zlib/crc32.c"
        "${DEPS_DIR}/zlib/deflate.c"
        "${DEPS_DIR}/zlib/infback.c"
        "${DEPS_DIR}/zlib/inffast.c"
        "${DEPS_DIR}/zlib/inflate.c"
        "${DEPS_DIR}/zlib/inftrees.c"
        "${DEPS_DIR}/zlib/trees.c"
        "${DEPS_DIR}/zlib/uncompr.c"
        "${DEPS_DIR}/zlib/zutil.c"
      )
      ;;
    bz2)
      algo_includes=(
        "-I${DEPS_DIR}/bz2"
      )
      algo_sources=(
        "${DEPS_DIR}/bz2/blocksort.c"
        "${DEPS_DIR}/bz2/huffman.c"
        "${DEPS_DIR}/bz2/crctable.c"
        "${DEPS_DIR}/bz2/randtable.c"
        "${DEPS_DIR}/bz2/compress.c"
        "${DEPS_DIR}/bz2/decompress.c"
        "${DEPS_DIR}/bz2/bzlib.c"
      )
      ;;
    lz4)
      algo_includes=(
        "-I${DEPS_DIR}/lz4/lib"
      )
      algo_sources=(
        "${DEPS_DIR}/lz4/lib/lz4.c"
        "${DEPS_DIR}/lz4/lib/lz4hc.c"
      )
      ;;
    xz)
      algo_includes=(
        "-I${DEPS_DIR}/xz/src/liblzma/api"
        "-I${DEPS_DIR}/xz/src/liblzma/common"
        "-I${DEPS_DIR}/xz/src/liblzma/check"
        "-I${DEPS_DIR}/xz/src/liblzma/lz"
        "-I${DEPS_DIR}/xz/src/liblzma/lzma"
        "-I${DEPS_DIR}/xz/src/liblzma/rangecoder"
        "-I${DEPS_DIR}/xz/src/liblzma/delta"
        "-I${DEPS_DIR}/xz/src/liblzma/simple"
        "-I${DEPS_DIR}/xz/src/common"
      )
      # XZ/LZMA source files (excluding multi-threaded files that require pthreads)
      algo_sources=()
      for f in "${DEPS_DIR}"/xz/src/liblzma/common/*.c \
               "${DEPS_DIR}"/xz/src/liblzma/check/*.c \
               "${DEPS_DIR}"/xz/src/liblzma/lz/*.c \
               "${DEPS_DIR}"/xz/src/liblzma/lzma/*.c \
               "${DEPS_DIR}"/xz/src/liblzma/rangecoder/*.c \
               "${DEPS_DIR}"/xz/src/liblzma/delta/*.c \
               "${DEPS_DIR}"/xz/src/liblzma/simple/*.c; do
        # Skip multi-threaded files
        case "$(basename "$f")" in
          *_mt.c|outqueue.c|hardware_cputhreads.c)
            continue
            ;;
        esac
        [ -f "$f" ] && algo_sources+=("$f")
      done
      # Add required defines for LZMA (disable threading, configure for WASM)
      algo_includes+=("-DHAVE_STDINT_H" "-DHAVE_STDBOOL_H" "-DHAVE_INTTYPES_H" "-DMYTHREAD_ENABLED=0" "-DTUKLIB_SYMBOL_PREFIX=lzma_")
      ;;
  esac

  # Build the WASM module
  local cmd=(
    emcc
    "$binding_cpp"
    "${algo_sources[@]}"
    "-I${PROJECT_ROOT}/src"
    "${algo_includes[@]}"
    "${COMMON_FLAGS[@]}"
    "-s" "EXPORTED_FUNCTIONS=['_malloc','_free','_cu_compress','_cu_decompress','_cu_stream_compress_create','_cu_stream_compress_write','_cu_stream_compress_finish','_cu_stream_compress_destroy','_cu_stream_decompress_create','_cu_stream_decompress_write','_cu_stream_decompress_finish','_cu_stream_decompress_destroy']"
    "-o" "$output_js"
  )

  if ! "${cmd[@]}" 2>&1; then
    echo -e "${RED}  Failed to build ${algo}${NC}"
    return 1
  fi

  # Optimize WASM if not in debug mode
  if [ "$DEBUG" = false ] && command -v wasm-opt &> /dev/null; then
    echo -e "  Optimizing ${algo}.wasm..."
    wasm-opt -Oz "$output_wasm" -o "$output_wasm"
  fi

  # Get file sizes
  local wasm_size=$(stat -f%z "$output_wasm" 2>/dev/null || stat -c%s "$output_wasm" 2>/dev/null || echo "?")
  echo -e "${GREEN}  ✓ ${algo}.wasm (${wasm_size} bytes)${NC}"
}

# Build each algorithm
for algo in $ALGORITHMS; do
  if [[ ! " $ALL_ALGORITHMS " =~ " $algo " ]]; then
    echo -e "${RED}Unknown algorithm: $algo${NC}"
    exit 1
  fi
  build_algorithm "$algo"
done

echo ""
echo -e "${BLUE}Generating TypeScript modules...${NC}"

# Run the inline script
if command -v npx &> /dev/null; then
  npx tsx "${SCRIPT_DIR}/inline-wasm.ts"
else
  echo -e "${YELLOW}Warning: npx not found. Run 'npm run build:inline' manually.${NC}"
fi

echo ""
echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║                    Build Complete                          ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════╝${NC}"
