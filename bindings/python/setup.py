from pathlib import Path
from skbuild import setup
import platform
import shutil

# Clean up existing shared library files
build_dir = Path("bindings/python/compress_utils")
for extension in ["compress_utils_py.*.so", "compress_utils_py.*.dylib", "compress_utils_py.*.pyd"]:
    for file in build_dir.glob(extension):
        try:
            print(f"Removing existing build artifact: {file}")
            file.unlink()
        except FileNotFoundError:
            print(f"File not found: {file}")
        except Exception as e:
            print(f"Error deleting {file}: {e}")

# Define CMake arguments
cmake_args = [
    '-DBUILD_PYTHON_BINDINGS=ON',
    '-DBUILD_C_BINDINGS=OFF',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DSCIKIT_BUILD=ON',
    '-DENABLE_TESTS=OFF',
]

# Use a consistent generator on Windows
if platform.system() == "Windows":
    cmake_args += ['-G', 'Visual Studio 17 2022', '-A', 'x64']

setup(
    name="compress-utils",
    version="0.4.0",
    description="Simple & high-performance compression utilities for Python",
    author="Nicolas Dupont",
    license="MIT",
    packages=['compress_utils'],
    package_dir={'compress_utils': 'bindings/python/compress_utils'},
    package_data={
        'compress_utils': [
            'README.md',
            'LICENSE',
            'compress_utils_py*.so',    # For Linux shared libraries
            'compress_utils_py*.dylib', # For macOS shared libraries
            'compress_utils_py*.pyd',   # For Windows shared libraries
        ]
    },
    cmake_args=cmake_args,
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C++",
        "Operating System :: OS Independent",
    ],
    python_requires=">=3.6",
    zip_safe=False,
)
