#!/usr/bin/env pwsh

# build.ps1 - A PowerShell script to build the `compress-utils` library on Windows.

<#
.SYNOPSIS
    A script to build the 'compress-utils' library on Windows.

.DESCRIPTION
    This script automates the build process for the 'compress-utils' library,
    providing options similar to the original Bash script.

.PARAMETER Clean
    Cleans the build directory before building.

.PARAMETER SkipTests
    Skips building and running tests.

.PARAMETER Debug
    Builds the project in debug mode.

.PARAMETER Algorithms
    Comma-separated list of algorithms to include.

.PARAMETER Languages
    Comma-separated list of language bindings to build.

.PARAMETER Cores
    Number of cores to use for building.

.EXAMPLE
    .\build.ps1 -Clean -Algorithms "zstd,zlib" -Languages "js"

.EXAMPLE
    .\build.ps1 -Algorithms "zlib"
#>

param(
    [switch]$Clean,
    [switch]$SkipTests,
    [switch]$Debug,
    [string]$Algorithms = "",
    [string]$Languages = "",
    [int]$Cores = 1,
    [switch]$Help
)

function Show-Usage {
    Write-Host "Usage: .\build.ps1 [OPTIONS]"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -Clean                    Clean the build directory before building."
    Write-Host "  -SkipTests                Skip building and running tests."
    Write-Host "  -Debug                    Build the project in debug mode."
    Write-Host "  -Algorithms LIST          Comma-separated list of algorithms to include. Default: all"
    Write-Host "                            Available algorithms: brotli, zstd, zlib, xz (lzma)"
    Write-Host "  -Languages LIST           Comma-separated list of language bindings to build. Default: all"
    Write-Host "                            Available languages: c, python, js, ts, wasm"
    Write-Host "  -Cores N                  Number of cores to use for building. Default: 1"
    Write-Host "  -Help                     Show this help message and exit."
    Write-Host ""
    Write-Host "Examples:"
    Write-Host "  .\build.ps1 -Clean -Algorithms 'zstd,zlib' -Languages 'js'"
    Write-Host "  .\build.ps1 -Algorithms 'zlib'"
    exit 1
}

if ($Help) {
    Show-Usage
}

# Initialize variables
$BUILD_DIR = "build"
$BUILD_MODE = "Release"
$ALGORITHMS_LIST = @()
$LANGUAGES_LIST = @()
$USING_EMSCRIPTEN = $false

# Set build mode
if ($Debug) {
    $BUILD_MODE = "Debug"
}

# Parse algorithms
if ($Algorithms -ne "") {
    $ALGORITHMS_LIST = $Algorithms.Split(',')
}

# Parse languages
if ($Languages -ne "") {
    $LANGUAGES_LIST = $Languages.Split(',')
}

# Check if CMake is installed
if (-not (Get-Command "cmake" -ErrorAction SilentlyContinue)) {
    Write-Error "CMake is required to build the project."
    exit 1
}

# Prepare CMake options as an array
$CMAKE_OPTIONS = @()

# Set the build mode
$CMAKE_OPTIONS += "-DCMAKE_BUILD_TYPE=$BUILD_MODE"

# Skip building and running tests if requested
if ($SkipTests) {
    $CMAKE_OPTIONS += "-DENABLE_TESTS=OFF"
}

# Handle algorithms
if ($ALGORITHMS_LIST.Count -gt 0) {
    # Disable all algorithms by default
    $CMAKE_OPTIONS += "-DINCLUDE_BROTLI=OFF"
    $CMAKE_OPTIONS += "-DINCLUDE_XZ=OFF"
    $CMAKE_OPTIONS += "-DINCLUDE_ZSTD=OFF"
    $CMAKE_OPTIONS += "-DINCLUDE_ZLIB=OFF"

    # Enable specified algorithms
    foreach ($algo in $ALGORITHMS_LIST) {
        switch ($algo.Trim().ToLower()) {
            "brotli" { $CMAKE_OPTIONS += "-DINCLUDE_BROTLI=ON" }
            "lzma"   { $CMAKE_OPTIONS += "-DINCLUDE_XZ=ON" }
            "xz"     { $CMAKE_OPTIONS += "-DINCLUDE_XZ=ON" }
            "zstd"   { $CMAKE_OPTIONS += "-DINCLUDE_ZSTD=ON" }
            "zlib"   { $CMAKE_OPTIONS += "-DINCLUDE_ZLIB=ON" }
            default  {
                Write-Error "Unknown algorithm: $algo"
                Show-Usage
            }
        }
    }
} else {
    # Enable all algorithms by default
    $CMAKE_OPTIONS += "-DINCLUDE_BROTLI=ON"
    $CMAKE_OPTIONS += "-DINCLUDE_XZ=ON"
    $CMAKE_OPTIONS += "-DINCLUDE_ZSTD=ON"
    $CMAKE_OPTIONS += "-DINCLUDE_ZLIB=ON"
}

# Handle language bindings
if ($LANGUAGES_LIST.Count -gt 0) {
    # Disable all bindings by default
    $CMAKE_OPTIONS += "-DBUILD_C_BINDINGS=OFF"
    $CMAKE_OPTIONS += "-DBUILD_PYTHON_BINDINGS=OFF"
    $CMAKE_OPTIONS += "-DBUILD_WASM_BINDINGS=OFF"

    # Enable specified bindings
    foreach ($lang in $LANGUAGES_LIST) {
        switch ($lang.Trim().ToLower()) {
            { $_ -in "js", "ts", "wasm" } {
                # Check if there are more than one language bindings
                if ($LANGUAGES_LIST.Count -gt 1) {
                    Write-Error "Warning: WebAssembly bindings cannot be built with other language bindings."
                    Write-Error "Please build the WebAssembly bindings separately."
                    exit 1
                }
                
                $CMAKE_OPTIONS += "-DBUILD_WASM_BINDINGS=ON"
                $USING_EMSCRIPTEN = $true
                $BUILD_DIR = "build_wasm"
            }
            "python" { 
                $CMAKE_OPTIONS += "-DBUILD_PYTHON_BINDINGS=ON" 
                $pythonPath = (Get-Command python -ErrorAction SilentlyContinue).Path
                if ($pythonPath) {
                    $CMAKE_OPTIONS += "-DPython3_EXECUTABLE=$pythonPath"
                }
            }
            "c" { $CMAKE_OPTIONS += "-DBUILD_C_BINDINGS=ON" }
            default {
                Write-Error "Unknown language binding: $lang"
                Show-Usage
            }
        }
    }
} else {
    # Enable all bindings by default except WASM (which requires Emscripten)
    $CMAKE_OPTIONS += "-DBUILD_C_BINDINGS=ON"
    $CMAKE_OPTIONS += "-DBUILD_PYTHON_BINDINGS=ON"
    $CMAKE_OPTIONS += "-DBUILD_WASM_BINDINGS=OFF"
    
    $pythonPath = (Get-Command python -ErrorAction SilentlyContinue).Path
    if ($pythonPath) {
        $CMAKE_OPTIONS += "-DPython3_EXECUTABLE=$pythonPath"
    }
}

# Check if emscripten is available when WASM is selected
if ($USING_EMSCRIPTEN) {
    # Check for emcc in the PATH
    $emccExists = Get-Command emcc -ErrorAction SilentlyContinue
    
    if (-not $emccExists) {
        Write-Error "Error: emcc (Emscripten compiler) not found."
        Write-Error "Please install and activate Emscripten SDK: https://emscripten.org/docs/getting_started/"
        exit 1
    }
    
    # Clear the build directory to avoid cross-compilation conflicts
    Remove-Item -Recurse -Force $BUILD_DIR -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $BUILD_DIR | Out-Null
    
    # Clean any existing external library builds to ensure they are built with emscripten
    Remove-Item -Recurse -Force "algorithms\*\build" -ErrorAction SilentlyContinue
}

# Clean the build directory if requested
if ($Clean) {
    Write-Host "Cleaning build directory..."
    Remove-Item -Recurse -Force "$BUILD_DIR" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "algorithms\*\build" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "bindings\*\build" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "dist" -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "algorithms\dist" -ErrorAction SilentlyContinue
}

# Create the build directory if it doesn't exist
if (-not (Test-Path "$BUILD_DIR")) {
    New-Item -ItemType Directory -Path "$BUILD_DIR" | Out-Null
}

# Move into the build directory
try {
    Push-Location "$BUILD_DIR"

    # Run CMake configuration
    Write-Host "Running CMake with options: $($CMAKE_OPTIONS -join ' ')"
    
    if ($USING_EMSCRIPTEN) {
        # Use emcmake for WebAssembly builds
        $emcmakeArgs = @("cmake", "..") + $CMAKE_OPTIONS
        Write-Host "Executing: emcmake $($emcmakeArgs -join ' ')"
        
        & emcmake @emcmakeArgs
        
        if ($LASTEXITCODE -ne 0) {
            Write-Error "emcmake failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
        
        # Build the project with emmake
        Write-Host "Building the project with $Cores cores..."
        & emmake make -j"$Cores"
        
        if ($LASTEXITCODE -ne 0) {
            Write-Error "emmake failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
        
        # Install the project
        Write-Host "Installing the project..."
        & emmake make install
        
        if ($LASTEXITCODE -ne 0) {
            Write-Error "emmake install failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
    } else {
        # Standard build for native targets
        & cmake .. @CMAKE_OPTIONS
        
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cmake configuration failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
        
        # Build the project with the specified number of cores
        Write-Host "Building the project with $Cores cores..."
        & cmake --build . --config $BUILD_MODE -- /m:$Cores
        
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cmake build failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
        
        # Install the project
        Write-Host "Installing the project..."
        & cmake --install . --config $BUILD_MODE
        
        if ($LASTEXITCODE -ne 0) {
            Write-Error "cmake install failed with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
    }

    # Run tests if not skipped
    if (-not $SkipTests) {
        Write-Host "Running tests..."
        & ctest -C $BUILD_MODE --output-on-failure
    }
} finally {
    # Return to the original directory
    Pop-Location
}

# Print the sizes of the built libraries
Write-Host ""
Write-Host "Sizes of the built libraries:"
Write-Host "-----------------------------"

if ($USING_EMSCRIPTEN) {
    # For WASM builds, look for .js and .wasm files
    Get-ChildItem -Path "dist\*" -Include "*.js", "*.wasm" -Recurse | ForEach-Object {
        $size = "{0:N2}" -f ($_.Length / 1KB)
        Write-Host "$($_.FullName): $size KB"
    }
} else {
    # For native builds, look for .lib and .dll files
    Get-ChildItem -Path "dist\*" -Include "compress_utils*.lib", "compress_utils*.dll", "compress_utils*.pyd" -Recurse | ForEach-Object {
        $size = "{0:N2}" -f ($_.Length / 1KB)
        Write-Host "$($_.FullName): $size KB"
    }
}