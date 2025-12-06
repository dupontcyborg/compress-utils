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

# CMake downloads sources here (from main build)
CMAKE_DEPS_DIR="${PROJECT_ROOT}/algorithms"

# Fallback directory for downloaded algorithm sources (if CMake hasn't run)
FALLBACK_DEPS_DIR="${WASM_DIR}/deps"

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
  rm -rf "$FALLBACK_DEPS_DIR"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Associative array to store source paths for each algorithm
declare -A ALGO_SOURCE_DIR

# Function to get the source directory for an algorithm
# First checks if CMake has downloaded it, otherwise downloads to fallback dir
get_algo_source_dir() {
  local algo=$1
  local cmake_path=""
  local cmake_name=""
  local git_url=""
  local git_tag=""

  case $algo in
    zstd)
      cmake_name="zstd_external"
      cmake_path="${CMAKE_DEPS_DIR}/zstd/build/src/${cmake_name}"
      git_url="https://github.com/facebook/zstd.git"
      git_tag="v1.5.6"
      ;;
    brotli)
      cmake_name="brotli_external"
      cmake_path="${CMAKE_DEPS_DIR}/brotli/build/src/${cmake_name}"
      git_url="https://github.com/google/brotli.git"
      git_tag="v1.1.0"
      ;;
    zlib)
      cmake_name="zlib_external"
      cmake_path="${CMAKE_DEPS_DIR}/zlib/build/src/${cmake_name}"
      git_url="https://github.com/madler/zlib.git"
      git_tag="v1.3.1"
      ;;
    bz2)
      cmake_name="bzip2_external"
      cmake_path="${CMAKE_DEPS_DIR}/bz2/build/src/${cmake_name}"
      git_url="https://gitlab.com/bzip2/bzip2.git"
      git_tag="bzip2-1.0.8"
      ;;
    lz4)
      cmake_name="lz4_external"
      cmake_path="${CMAKE_DEPS_DIR}/lz4/build/src/${cmake_name}"
      git_url="https://github.com/lz4/lz4.git"
      git_tag="v1.10.0"
      ;;
    xz)
      cmake_name="xz_external"
      cmake_path="${CMAKE_DEPS_DIR}/xz/build/src/${cmake_name}"
      git_url="https://github.com/tukaani-project/xz.git"
      git_tag="v5.6.3"
      ;;
    *)
      echo ""
      return 1
      ;;
  esac

  # Check if CMake has already downloaded the sources
  if [ -d "$cmake_path" ]; then
    echo "$cmake_path"
    return 0
  fi

  # Fall back to downloading
  local fallback_path="${FALLBACK_DEPS_DIR}/${algo}"
  if [ ! -d "$fallback_path" ]; then
    mkdir -p "$FALLBACK_DEPS_DIR"
    echo -e "  Downloading ${algo}..." >&2
    git clone --depth 1 --branch "$git_tag" "$git_url" "$fallback_path" 2>/dev/null || true
  fi
  echo "$fallback_path"
}

# Function to check and setup algorithm sources
setup_algorithm_sources() {
  echo -e "${BLUE}Checking algorithm sources...${NC}"

  for algo in $ALGORITHMS; do
    local source_dir
    source_dir=$(get_algo_source_dir "$algo")
    if [ -n "$source_dir" ] && [ -d "$source_dir" ]; then
      ALGO_SOURCE_DIR[$algo]="$source_dir"
      echo -e "  ${GREEN}✓${NC} ${algo}: ${source_dir}"
    else
      echo -e "  ${RED}✗${NC} ${algo}: sources not found"
    fi
  done

  echo -e "${GREEN}  Algorithm sources ready${NC}"
  echo ""
}

# Setup algorithm sources
setup_algorithm_sources

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

  # Get the source directory for this algorithm
  local src_dir="${ALGO_SOURCE_DIR[$algo]}"
  if [ -z "$src_dir" ] || [ ! -d "$src_dir" ]; then
    echo -e "${RED}  Error: Source directory not found for ${algo}${NC}"
    return 1
  fi

  # Algorithm-specific source files and include paths
  local algo_sources=()
  local algo_includes=()
  local algo_libs=()

  case $algo in
    zstd)
      algo_includes=(
        "-I${src_dir}/lib"
        "-I${src_dir}/lib/common"
      )
      # Collect zstd source files (glob expansion happens here)
      algo_sources=()
      for f in "${src_dir}"/lib/common/*.c "${src_dir}"/lib/compress/*.c "${src_dir}"/lib/decompress/*.c; do
        [ -f "$f" ] && algo_sources+=("$f")
      done
      ;;
    brotli)
      algo_includes=(
        "-I${src_dir}/c/include"
      )
      algo_sources=()
      for f in "${src_dir}"/c/common/*.c "${src_dir}"/c/enc/*.c "${src_dir}"/c/dec/*.c; do
        [ -f "$f" ] && algo_sources+=("$f")
      done
      ;;
    zlib)
      algo_includes=(
        "-I${src_dir}"
      )
      # zlib source files (excluding gz* files that require filesystem, and test/example files)
      algo_sources=(
        "${src_dir}/adler32.c"
        "${src_dir}/compress.c"
        "${src_dir}/crc32.c"
        "${src_dir}/deflate.c"
        "${src_dir}/infback.c"
        "${src_dir}/inffast.c"
        "${src_dir}/inflate.c"
        "${src_dir}/inftrees.c"
        "${src_dir}/trees.c"
        "${src_dir}/uncompr.c"
        "${src_dir}/zutil.c"
      )
      ;;
    bz2)
      algo_includes=(
        "-I${src_dir}"
      )
      algo_sources=(
        "${src_dir}/blocksort.c"
        "${src_dir}/huffman.c"
        "${src_dir}/crctable.c"
        "${src_dir}/randtable.c"
        "${src_dir}/compress.c"
        "${src_dir}/decompress.c"
        "${src_dir}/bzlib.c"
      )
      ;;
    lz4)
      algo_includes=(
        "-I${src_dir}/lib"
      )
      algo_sources=(
        "${src_dir}/lib/lz4.c"
        "${src_dir}/lib/lz4hc.c"
      )
      ;;
    xz)
      algo_includes=(
        "-I${src_dir}/src/liblzma/api"
        "-I${src_dir}/src/liblzma/common"
        "-I${src_dir}/src/liblzma/check"
        "-I${src_dir}/src/liblzma/lz"
        "-I${src_dir}/src/liblzma/lzma"
        "-I${src_dir}/src/liblzma/rangecoder"
        "-I${src_dir}/src/liblzma/delta"
        "-I${src_dir}/src/liblzma/simple"
        "-I${src_dir}/src/common"
      )
      # XZ/LZMA source files (excluding multi-threaded files that require pthreads)
      algo_sources=()
      for f in "${src_dir}"/src/liblzma/common/*.c \
               "${src_dir}"/src/liblzma/check/*.c \
               "${src_dir}"/src/liblzma/lz/*.c \
               "${src_dir}"/src/liblzma/lzma/*.c \
               "${src_dir}"/src/liblzma/rangecoder/*.c \
               "${src_dir}"/src/liblzma/delta/*.c \
               "${src_dir}"/src/liblzma/simple/*.c; do
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
