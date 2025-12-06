#!/usr/bin/env bash
#
# Build WASM modules for compress-utils.
#
# This script compiles the C++ compression algorithms to WebAssembly using Emscripten.
# Each algorithm is built as a separate WASM module for tree-shakeability.
#
# Prerequisites:
#   - Emscripten SDK installed and activated (emcc in PATH)
#   - Algorithm dependencies built (run main build.sh first)
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
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Common Emscripten flags
COMMON_FLAGS=(
  "-s" "WASM=1"
  "-s" "MODULARIZE=1"
  "-s" "EXPORT_ES6=1"
  "-s" "ALLOW_MEMORY_GROWTH=1"
  "-s" "MALLOC=emmalloc"
  "-s" "FILESYSTEM=0"
  "-s" "ENVIRONMENT='web,node'"
  "-s" "EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
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
        "-I${PROJECT_ROOT}/algorithms/zstd/lib"
        "-I${PROJECT_ROOT}/algorithms/zstd/lib/common"
      )
      # Include all zstd source files
      algo_sources=(
        "${PROJECT_ROOT}/algorithms/zstd/lib/common/"*.c
        "${PROJECT_ROOT}/algorithms/zstd/lib/compress/"*.c
        "${PROJECT_ROOT}/algorithms/zstd/lib/decompress/"*.c
      )
      ;;
    brotli)
      algo_includes=(
        "-I${PROJECT_ROOT}/algorithms/brotli/c/include"
      )
      algo_sources=(
        "${PROJECT_ROOT}/algorithms/brotli/c/common/"*.c
        "${PROJECT_ROOT}/algorithms/brotli/c/enc/"*.c
        "${PROJECT_ROOT}/algorithms/brotli/c/dec/"*.c
      )
      ;;
    zlib)
      algo_includes=(
        "-I${PROJECT_ROOT}/algorithms/zlib"
      )
      algo_sources=(
        "${PROJECT_ROOT}/algorithms/zlib/"*.c
      )
      ;;
    bz2)
      algo_includes=(
        "-I${PROJECT_ROOT}/algorithms/bz2"
      )
      algo_sources=(
        "${PROJECT_ROOT}/algorithms/bz2/blocksort.c"
        "${PROJECT_ROOT}/algorithms/bz2/huffman.c"
        "${PROJECT_ROOT}/algorithms/bz2/crctable.c"
        "${PROJECT_ROOT}/algorithms/bz2/randtable.c"
        "${PROJECT_ROOT}/algorithms/bz2/compress.c"
        "${PROJECT_ROOT}/algorithms/bz2/decompress.c"
        "${PROJECT_ROOT}/algorithms/bz2/bzlib.c"
      )
      ;;
    lz4)
      algo_includes=(
        "-I${PROJECT_ROOT}/algorithms/lz4/lib"
      )
      algo_sources=(
        "${PROJECT_ROOT}/algorithms/lz4/lib/lz4.c"
        "${PROJECT_ROOT}/algorithms/lz4/lib/lz4hc.c"
      )
      ;;
    xz)
      algo_includes=(
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/api"
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/common"
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/check"
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/lz"
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/lzma"
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/rangecoder"
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/delta"
        "-I${PROJECT_ROOT}/algorithms/xz/src/liblzma/simple"
        "-I${PROJECT_ROOT}/algorithms/xz/src/common"
      )
      # XZ is complex, uses autotools - may need pre-configuration
      # For now, we'll handle this separately
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
    "-s" "EXPORTED_FUNCTIONS=['_malloc','_free','_compress','_decompress','_stream_compress_create','_stream_compress_write','_stream_compress_finish','_stream_compress_destroy','_stream_decompress_create','_stream_decompress_write','_stream_decompress_finish','_stream_decompress_destroy']"
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
