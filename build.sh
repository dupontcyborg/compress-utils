#!/bin/bash

# build.sh - A script to build the `compress-utils` library on Unix-like systems.

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Initialize variables
BUILD_DIR="build"
CLEAN_BUILD=false
SKIP_TESTS=false
SKIP_WASM_TESTS=false
BUILD_MODE="Release"
CORES=1
ALGORITHMS=()
LANGUAGES=()

# Function to display usage instructions
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --clean                    Clean the build directory before building."
    echo "  --skip-tests               Skip building and running all tests."
    echo "  --skip-wasm-tests          Skip WASM-specific tests (unit, browser, treeshake)."
    echo "  --debug                    Build the project in debug mode."
    echo "  --algorithms=LIST          Comma-separated list of algorithms to include. Default: all"
    echo "                             Available algorithms: brotli, bz2 (bzip2), lz4, zstd, zlib, xz (lzma)"
    echo "  --languages=LIST           Comma-separated list of language bindings to build. Default: all"
    echo "                             Available languages: c, python, wasm (or js/ts)"
    echo "  --cores=N                  Number of cores to use for building. Default: 1"
    echo "  -h, --help                 Show this help message and exit."
    echo ""
    echo "Examples:"
    echo "  $0 --clean --algorithms=zstd,zlib --languages=wasm"
    echo "  $0 --languages=wasm --skip-wasm-tests"
    echo "  $0 --algorithms=zlib"
    echo "  $0 --clean --languages=c,python,wasm"
    exit 1
}

# Parse command-line options
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN_BUILD=true
            ;;
        --skip-tests)
            SKIP_TESTS=true
            ;;
        --skip-wasm-tests)
            SKIP_WASM_TESTS=true
            ;;
        --debug)
            BUILD_MODE="Debug"
            ;;
        --algorithms=*)
            IFS=',' read -ra ALGORITHMS <<< "${1#*=}"
            ;;
        --languages=*)
            IFS=',' read -ra LANGUAGES <<< "${1#*=}"
            ;;
        --cores=*)
            CORES="${1#*=}"
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
    shift
done

# Check if CMake is installed
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}CMake is required to build the project.${NC}"
    exit 1
fi

# Determine which languages to build
BUILD_C=false
BUILD_PYTHON=false
BUILD_WASM=false

if [ ${#LANGUAGES[@]} -gt 0 ]; then
    for lang in "${LANGUAGES[@]}"; do
        case $lang in
            c)
                BUILD_C=true
                ;;
            python)
                BUILD_PYTHON=true
                ;;
            wasm|js|ts)
                BUILD_WASM=true
                ;;
            *)
                echo -e "${RED}Unknown language binding: $lang${NC}"
                usage
                ;;
        esac
    done
else
    # Enable all bindings by default
    BUILD_C=true
    BUILD_PYTHON=true
    BUILD_WASM=true
fi

# Check for Emscripten if WASM build is requested
EMSCRIPTEN_AVAILABLE=false
if [ "$BUILD_WASM" = true ]; then
    if command -v emcc &> /dev/null; then
        EMSCRIPTEN_AVAILABLE=true
        echo -e "${GREEN}Emscripten found: $(emcc --version | head -1)${NC}"
    else
        echo -e "${YELLOW}Warning: Emscripten (emcc) not found. WASM bindings will be skipped.${NC}"
        echo -e "${YELLOW}To install Emscripten:${NC}"
        echo -e "${YELLOW}  git clone https://github.com/emscripten-core/emsdk.git${NC}"
        echo -e "${YELLOW}  cd emsdk && ./emsdk install latest && ./emsdk activate latest${NC}"
        echo -e "${YELLOW}  source emsdk_env.sh${NC}"
        BUILD_WASM=false
    fi
fi

# Clean the build directory if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo -e "${BLUE}Cleaning build directories...${NC}"
    rm -rf "$BUILD_DIR"

    # Remove the build directories under `algorithms/`
    rm -rf algorithms/*/build

    # Remove the build directories under `bindings/`
    rm -rf bindings/*/build

    # Remove the `dist/` directory
    rm -rf dist

    # Remove the `algorithms/dist` directory
    rm -rf algorithms/dist

    # Clean WASM build artifacts
    rm -rf bindings/wasm/dist
    rm -rf bindings/wasm/wasm-build
    rm -rf bindings/wasm/node_modules
fi

# Create the build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Prepare CMake options
CMAKE_OPTIONS=""

# Set the build mode
CMAKE_OPTIONS="$CMAKE_OPTIONS -DCMAKE_BUILD_TYPE=$BUILD_MODE"

# Skip building and running tests if requested
if [ "$SKIP_TESTS" = true ]; then
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DENABLE_TESTS=OFF"
fi

# Handle algorithms
SELECTED_ALGORITHMS=""
if [ ${#ALGORITHMS[@]} -gt 0 ]; then
    # Disable all algorithms by default
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_BROTLI=OFF -DINCLUDE_BZ2=OFF -DINCLUDE_LZ4=OFF -DINCLUDE_XZ=OFF -DINCLUDE_ZSTD=OFF -DINCLUDE_ZLIB=OFF"
    # Enable specified algorithms
    for algo in "${ALGORITHMS[@]}"; do
        case $algo in
            brotli)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_BROTLI=ON"
                SELECTED_ALGORITHMS="$SELECTED_ALGORITHMS brotli"
                ;;
            bz2|bzip2)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_BZ2=ON"
                SELECTED_ALGORITHMS="$SELECTED_ALGORITHMS bz2"
                ;;
            lz4)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_LZ4=ON"
                SELECTED_ALGORITHMS="$SELECTED_ALGORITHMS lz4"
                ;;
            lzma)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_XZ=ON"
                SELECTED_ALGORITHMS="$SELECTED_ALGORITHMS xz"
                ;;
            xz)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_XZ=ON"
                SELECTED_ALGORITHMS="$SELECTED_ALGORITHMS xz"
                ;;
            zstd)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_ZSTD=ON"
                SELECTED_ALGORITHMS="$SELECTED_ALGORITHMS zstd"
                ;;
            zlib)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_ZLIB=ON"
                SELECTED_ALGORITHMS="$SELECTED_ALGORITHMS zlib"
                ;;
            *)
                echo -e "${RED}Unknown algorithm: $algo${NC}"
                usage
                ;;
        esac
    done
else
    # Enable all algorithms by default
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_BROTLI=ON -DINCLUDE_BZ2=ON -DINCLUDE_LZ4=ON -DINCLUDE_XZ=ON -DINCLUDE_ZSTD=ON -DINCLUDE_ZLIB=ON"
    SELECTED_ALGORITHMS="zstd brotli zlib bz2 lz4 xz"
fi

# Handle language bindings for CMake (C and Python only - WASM is separate)
CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_C_BINDINGS=OFF -DBUILD_PYTHON_BINDINGS=OFF"

if [ "$BUILD_C" = true ]; then
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_C_BINDINGS=ON"
fi

if [ "$BUILD_PYTHON" = true ]; then
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_PYTHON_BINDINGS=ON"
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DPython3_EXECUTABLE=$(which python)"
fi

# ============================================================================
# Build C/C++ and Python bindings with CMake
# ============================================================================

if [ "$BUILD_C" = true ] || [ "$BUILD_PYTHON" = true ]; then
    echo ""
    echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║           Building C/C++ Library                           ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"

    # Move into the build directory
    cd "$BUILD_DIR"

    # Run CMake configuration
    echo -e "${BLUE}Running CMake with options: $CMAKE_OPTIONS${NC}"
    cmake .. $CMAKE_OPTIONS

    # Build the project with the specified number of cores
    echo -e "${BLUE}Building the project with $CORES cores...${NC}"
    cmake --build . -j"$CORES"

    # Install the project (this will trigger the CMake install() commands)
    echo -e "${BLUE}Installing the project...${NC}"
    cmake --install .

    # Run tests if not skipped
    if [ "$SKIP_TESTS" = false ]; then
        echo -e "${BLUE}Running C/C++ tests...${NC}"
        ctest --output-on-failure
    fi

    # Return to the original directory
    cd ..
fi

# ============================================================================
# Build WASM bindings with Emscripten
# ============================================================================

if [ "$BUILD_WASM" = true ] && [ "$EMSCRIPTEN_AVAILABLE" = true ]; then
    echo ""
    echo -e "${BLUE}╔════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║           Building WASM Bindings                           ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════════════════════════╝${NC}"

    WASM_DIR="bindings/wasm"

    # Check if npm is available
    if ! command -v npm &> /dev/null; then
        echo -e "${RED}npm is required to build WASM bindings.${NC}"
        exit 1
    fi

    # Install npm dependencies
    echo -e "${BLUE}Installing npm dependencies...${NC}"
    cd "$WASM_DIR"
    npm install

    # Build WASM modules
    echo -e "${BLUE}Building WASM modules with Emscripten...${NC}"
    if [ "$BUILD_MODE" = "Debug" ]; then
        ./scripts/build-wasm.sh --algorithms="${SELECTED_ALGORITHMS// /,}" --debug
    else
        ./scripts/build-wasm.sh --algorithms="${SELECTED_ALGORITHMS// /,}"
    fi

    # Check if WASM build succeeded
    if [ $? -ne 0 ]; then
        echo -e "${RED}WASM build failed.${NC}"
        cd ../..
        exit 1
    fi

    # Build TypeScript
    echo -e "${BLUE}Compiling TypeScript...${NC}"
    npm run build:ts

    # Run WASM tests if not skipped
    if [ "$SKIP_TESTS" = false ] && [ "$SKIP_WASM_TESTS" = false ]; then
        echo -e "${BLUE}Running WASM unit tests...${NC}"
        npm test

        echo -e "${BLUE}Running tree-shaking validation tests...${NC}"
        npm run test:treeshake

        # Browser tests require a display, skip in CI unless configured
        if [ -z "$CI" ]; then
            echo -e "${BLUE}Running browser tests...${NC}"
            npm run test:browser
        else
            echo -e "${YELLOW}Skipping browser tests in CI environment.${NC}"
        fi
    fi

    cd ../..
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║                    Build Complete                          ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""

# Print what was built
echo -e "${BLUE}Built components:${NC}"
if [ "$BUILD_C" = true ]; then
    echo -e "  ${GREEN}✓${NC} C/C++ library"
fi
if [ "$BUILD_PYTHON" = true ]; then
    echo -e "  ${GREEN}✓${NC} Python bindings"
fi
if [ "$BUILD_WASM" = true ] && [ "$EMSCRIPTEN_AVAILABLE" = true ]; then
    echo -e "  ${GREEN}✓${NC} WASM/JavaScript bindings"
elif [ "$BUILD_WASM" = true ]; then
    echo -e "  ${YELLOW}⊘${NC} WASM/JavaScript bindings (Emscripten not available)"
fi
echo ""

# Print the sizes of the built libraries
echo -e "${BLUE}Sizes of the built libraries:${NC}"
echo "-----------------------------"

# C/C++ libraries
if [ -d "dist/cpp" ]; then
    find dist/cpp -type f \( -name "*.so" -o -name "*.dylib" -o -name "*.a" -o -name "*.dll" -o -name "*.lib" \) -exec du -sh {} + 2>/dev/null | awk '{print "  " $2 ": " $1}'
fi

# Python bindings
if [ -d "dist/python" ]; then
    find dist/python -type f -name "*.so" -exec du -sh {} + 2>/dev/null | awk '{print "  " $2 ": " $1}'
fi

# WASM bindings
if [ -d "bindings/wasm/wasm-build" ]; then
    echo ""
    echo -e "${BLUE}WASM module sizes:${NC}"
    find bindings/wasm/wasm-build -type f -name "*.wasm" -exec du -sh {} + 2>/dev/null | awk '{print "  " $2 ": " $1}'
fi

echo ""
echo -e "${GREEN}Done!${NC}"
