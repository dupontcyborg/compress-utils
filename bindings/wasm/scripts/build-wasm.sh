#!/usr/bin/env bash
#
# Build WASM modules for compress-utils using CMake + Emscripten.
#
# This script compiles the C++ compression algorithms to WebAssembly using Emscripten.
# Each algorithm is built as a separate WASM module for tree-shakeability.
#
# Prerequisites:
#   - Emscripten SDK installed and activated (emcc in PATH)
#   - CMake 3.17+
#
# Usage:
#   ./scripts/build-wasm.sh [options]
#
# Options:
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

# Build directories
CMAKE_BUILD_DIR="${WASM_DIR}/cmake-build"
WASM_OUTPUT_DIR="${WASM_DIR}/wasm-build"
GENERATED_DIR="${WASM_DIR}/src/algorithms"
DIST_DIR="${WASM_DIR}/dist/algorithms"

# All supported algorithms
ALL_ALGORITHMS="zstd brotli zlib bz2 lz4 xz"

# Default options
DEBUG=false
CLEAN=false

# Parse arguments
for arg in "$@"; do
  case $arg in
    --debug)
      DEBUG=true
      ;;
    --clean)
      CLEAN=true
      ;;
    --help)
      head -20 "$0" | tail -18
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

# Verify emcmake is available
if ! command -v emcmake &> /dev/null; then
  echo -e "${RED}Error: emcmake not found. Please install and activate the Emscripten SDK.${NC}"
  exit 1
fi

# Verify CMake is available
if ! command -v cmake &> /dev/null; then
  echo -e "${RED}Error: cmake not found. Please install CMake 3.17+.${NC}"
  exit 1
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║        compress-utils WASM Build (CMake)                   ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "Emscripten version: $(emcc --version | head -1)"
echo -e "CMake version: $(cmake --version | head -1)"
echo -e "Debug mode: ${DEBUG}"
echo ""

# Clean if requested
if [ "$CLEAN" = true ]; then
  echo -e "${YELLOW}Cleaning build artifacts...${NC}"
  rm -rf "$CMAKE_BUILD_DIR"
  rm -rf "$WASM_OUTPUT_DIR"
  for algo in $ALL_ALGORITHMS; do
    rm -f "${GENERATED_DIR}/${algo}/wasm.generated.js"
    rm -f "${DIST_DIR}/${algo}/wasm.generated.js"
  done
fi

# Create build directory
mkdir -p "$CMAKE_BUILD_DIR"

# Configure with CMake + Emscripten
echo -e "${BLUE}Configuring with CMake...${NC}"
cd "$CMAKE_BUILD_DIR"

CMAKE_ARGS=""
if [ "$DEBUG" = true ]; then
  CMAKE_ARGS="-DWASM_DEBUG=ON"
fi

emcmake cmake $CMAKE_ARGS ..

# Build
echo ""
echo -e "${BLUE}Building WASM modules...${NC}"
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Check results
echo ""
echo -e "${BLUE}Build results:${NC}"

SUCCESS=true
for algo in $ALL_ALGORITHMS; do
  output_js="${WASM_OUTPUT_DIR}/${algo}.js"
  if [ -f "$output_js" ]; then
    size=$(stat -f%z "$output_js" 2>/dev/null || stat -c%s "$output_js" 2>/dev/null || echo "?")
    echo -e "  ${GREEN}✓${NC} ${algo}.js (${size} bytes)"
  else
    echo -e "  ${RED}✗${NC} ${algo}.js not found"
    SUCCESS=false
  fi
done

if [ "$SUCCESS" = false ]; then
  echo ""
  echo -e "${RED}Some WASM modules failed to build.${NC}"
  exit 1
fi

# Copy to src directories
echo ""
echo -e "${BLUE}Copying WASM modules to source directories...${NC}"

for algo in $ALL_ALGORITHMS; do
  src_dest="${GENERATED_DIR}/${algo}/wasm.generated.js"
  if [ -f "${WASM_OUTPUT_DIR}/${algo}.js" ]; then
    cp "${WASM_OUTPUT_DIR}/${algo}.js" "$src_dest"
    echo -e "  ${GREEN}✓${NC} Copied to src/algorithms/${algo}/"
  fi
done

# Also copy to dist if it exists
if [ -d "$DIST_DIR" ]; then
  echo ""
  echo -e "${BLUE}Copying WASM modules to dist directories...${NC}"
  for algo in $ALL_ALGORITHMS; do
    dist_dest="${DIST_DIR}/${algo}/wasm.generated.js"
    if [ -f "${WASM_OUTPUT_DIR}/${algo}.js" ] && [ -d "$(dirname "$dist_dest")" ]; then
      cp "${WASM_OUTPUT_DIR}/${algo}.js" "$dist_dest"
      echo -e "  ${GREEN}✓${NC} Copied to dist/algorithms/${algo}/"
    fi
  done
fi

echo ""
echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║                    Build Complete                          ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════╝${NC}"
