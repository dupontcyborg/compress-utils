#!/bin/bash

# build.sh - A script to build the `compress-utils` library on Unix-like systems.

# Initialize variables
BUILD_DIR="build"
CLEAN_BUILD=false
SKIP_TESTS=false
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
    echo "  --skip-tests               Skip building and running tests."
    echo "  --debug                    Build the project in debug mode."
    echo "  --algorithms=LIST          Comma-separated list of algorithms to include. Default: all"
    echo "                             Available algorithms: brotli, zstd, zlib, xz (lzma)"
    echo "  --languages=LIST           Comma-separated list of language bindings to build. Default: all"
    echo "                             Available languages: c, js, python"
    echo "  --cores=N                  Number of cores to use for building. Default: 1"
    echo "  -h, --help                 Show this help message and exit."
    echo ""
    echo "Examples:"
    echo "  $0 --clean --algorithms=zstd,zlib --languages=js"
    echo "  $0 --algorithms=zlib"
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
    echo "CMake is required to build the project."
    exit 1
fi

# Clean the build directory if requested
if [ "$CLEAN_BUILD" = true ]; then
    echo "Cleaning build directory..."
    rm -rf "$BUILD_DIR"

    # Remove the build directories under `algorithms/`
    rm -rf algorithms/*/build

    # Remove the build directories under `bindings/`
    rm -rf bindings/*/build

    # Remove the `dist/` directory
    rm -rf dist

    # Remove the `algorithms/dist` directory
    rm -rf algorithms/dist
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
if [ ${#ALGORITHMS[@]} -gt 0 ]; then
    # Disable all algorithms by default
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_BROTLI=OFF -DINCLUDE_XZ=ON -DINCLUDE_ZSTD=OFF -DINCLUDE_ZLIB=OFF"
    # Enable specified algorithms
    for algo in "${ALGORITHMS[@]}"; do
        case $algo in
            brotli)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_BROTLI=ON"
                ;;
            lzma)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_XZ=ON"
                ;;
            xz)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_XZ=ON"
                ;;
            zstd)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_ZSTD=ON"
                ;;
            zlib)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_ZLIB=ON"
                ;;
            *)
                echo "Unknown algorithm: $algo"
                usage
                ;;
        esac
    done
else
    # Enable all algorithms by default
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DINCLUDE_BROTLI=ON -DINCLUDE_XZ=ON -DINCLUDE_ZSTD=ON -DINCLUDE_ZLIB=ON"
fi

# Handle language bindings
if [ ${#LANGUAGES[@]} -gt 0 ]; then
    # Disable all bindings by default
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_JS_TS_BINDINGS=OFF -DBUILD_PYTHON_BINDINGS=OFF"
    # Enable specified bindings
    for lang in "${LANGUAGES[@]}"; do
        case $lang in
            js)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_JS_TS_BINDINGS=ON"
                ;;
            ts)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_JS_TS_BINDINGS=ON"
                ;;
            python)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_PYTHON_BINDINGS=ON"
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DPython3_EXECUTABLE=$(which python)"
                ;;
            c)
                CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_C_BINDINGS=ON"
                ;;
            *)
                echo "Unknown language binding: $lang"
                usage
                ;;
        esac
    done
else
    # Enable all bindings by default
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DBUILD_C_BINDINGS=ON -DBUILD_PYTHON_BINDINGS=ON"
    CMAKE_OPTIONS="$CMAKE_OPTIONS -DPython3_EXECUTABLE=$(which python)"
fi

# Move into the build directory
cd "$BUILD_DIR"

# Run CMake configuration
echo "Running CMake with options: $CMAKE_OPTIONS"
cmake .. $CMAKE_OPTIONS

# Build the project with the specified number of cores
echo "Building the project with $CORES cores..."
cmake --build . -j"$CORES"

# Install the project (this will trigger the CMake install() commands)
echo "Installing the project..."
cmake --install .

# Run tests if not skipped
if [ "$SKIP_TESTS" = false ]; then
    echo "Running tests..."
    ctest --output-on-failure
fi

# Return to the original directory
cd ..

# Print the sizes of the built libraries
echo ""
echo "Sizes of the built libraries:"
echo "-----------------------------"
find dist/** -type f -name "*compress_utils*" -exec du -sh {} + | awk '{print $2 ": " $1}'