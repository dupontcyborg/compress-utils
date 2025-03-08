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

# Read README for the long description
this_directory = Path(__file__).parent
long_description = (this_directory / "README.md").read_text()

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
    use_scm_version=True,
    description="Simple & high-performance compression utilities for Python",
    long_description=long_description,
    long_description_content_type="text/markdown",
    author="Nicolas Dupont",
    url="https://github.com/dupontcyborg/compress-utils",
    project_urls={
        "Bug Tracker": "https://github.com/dupontcyborg/compress-utils/issues",
        "Documentation": "https://github.com/dupontcyborg/compress-utils#readme",
        "Source Code": "https://github.com/dupontcyborg/compress-utils",
    },
    keywords="compression, zlib, brotli, zstd, xz, lzma, utility, performance",
    license="MIT",
    packages=['compress_utils'],
    package_dir={'compress_utils': 'bindings/python/compress_utils'},
    package_data={
        'compress_utils': [
            'README.md',
            'LICENSE',
            'compress_utils_py*.so',
            'compress_utils_py*.dylib',
            'compress_utils_py*.pyd',
        ]
    },
    setup_requires=["setuptools_scm"],  # Add this
    cmake_args=cmake_args,
    classifiers=[
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.6",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: C++",
        "Operating System :: OS Independent",
        "Topic :: System :: Archiving :: Compression",
        "Topic :: Software Development :: Libraries",
    ],
    python_requires=">=3.6",
    zip_safe=False,
)
