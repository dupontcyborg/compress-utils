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

# .PARAMETER Debug
#     Builds the project in debug mode.

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
    # [switch]$Debug,
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
    # Write-Host "  -Debug                    Build the project in debug mode."
    Write-Host "  -Algorithms LIST          Comma-separated list of algorithms to include. Default: all"
    Write-Host "                            Available algorithms: brotli, zstd, zlib, xz (lzma)"
    Write-Host "  -Languages LIST           Comma-separated list of language bindings to build. Default: all"
    Write-Host "                            Available languages: c, js, python"
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

# # Set build mode
# if ($Debug) {
#     $BUILD_MODE = "Debug"
# }

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
    $CMAKE_OPTIONS += "-DBUILD_JS_TS_BINDINGS=OFF"
    $CMAKE_OPTIONS += "-DBUILD_PYTHON_BINDINGS=OFF"

    # Enable specified bindings
    foreach ($lang in $LANGUAGES_LIST) {
        switch ($lang.Trim().ToLower()) {
            "js"     { $CMAKE_OPTIONS += "-DBUILD_JS_TS_BINDINGS=ON" }
            "ts"     { $CMAKE_OPTIONS += "-DBUILD_JS_TS_BINDINGS=ON" }
            "python" { $CMAKE_OPTIONS += "-DBUILD_PYTHON_BINDINGS=ON" }
            "c"      { $CMAKE_OPTIONS += "-DBUILD_C_BINDINGS=ON" }
            "none"   { $CMAKE_OPTIONS += "-DBUILD_C_BINDINGS=OFF" }
            default  {
                Write-Error "Unknown language binding: $lang"
                Show-Usage
            }
        }
    }
} else {
    # Enable all bindings by default
    $CMAKE_OPTIONS += "-DBUILD_C_BINDINGS=ON -DBUILD_PYTHON_BINDINGS=ON"
}

# Move into the build directory
Set-Location "$BUILD_DIR"

# Combine the arguments for cmake
$cmakeArgs = @("..") + $CMAKE_OPTIONS

# Run CMake configuration
Write-Host "Running CMake with options: $($cmakeArgs -join ' ')"
cmake @cmakeArgs

# Build the project with the specified number of cores
Write-Host "Building the project with $Cores cores..."
cmake --build . --config $BUILD_MODE -- /m:$Cores

# Install the project (this will trigger the CMake install() commands)
Write-Host "Installing the project..."
cmake --install . --config $BUILD_MODE

# Run tests if not skipped
if (-not $SkipTests) {
    Write-Host "Running tests..."
    ctest -C $BUILD_MODE --output-on-failure
}

# Return to the original directory
Set-Location ..

# Print the sizes of the built libraries
Write-Host ""
Write-Host "Sizes of the built libraries:"
Write-Host "-----------------------------"
Get-ChildItem -Path "dist\*\lib\*" -Filter "compress_utils*.lib" -Recurse | ForEach-Object {
    $size = "{0:N2}" -f ($_.Length / 1MB)
    Write-Host "$($_.FullName): $size MB"
}
Get-ChildItem -Path "dist\*\*\lib\*" -Filter "compress_utils*.lib" -Recurse | ForEach-Object {
    $size = "{0:N2}" -f ($_.Length / 1MB)
    Write-Host "$($_.FullName): $size MB"
}
Get-ChildItem -Path "dist\*\*\lib\*" -Filter "compress_utils*.dll" -Recurse | ForEach-Object {
    $size = "{0:N2}" -f ($_.Length / 1MB)
    Write-Host "$($_.FullName): $size MB"
}