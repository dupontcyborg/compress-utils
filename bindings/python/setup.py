from skbuild import setup
import os

# Define an install prefix within the build directory
# install_prefix = os.path.join("_skbuild", "cmake-install")

setup(
    name="compress_utils_py",
    version="0.4.0",
    description="Simple & high-performance compression utilities for Python",
    author="Nicolas Dupont",
    license="MIT",
    packages=['compress_utils_py'],
    package_dir={'compress_utils_py': 'bindings/python'},
    cmake_args=[
        '-DBUILD_PYTHON_BINDINGS=ON',
        '-DBUILD_C_BINDINGS=OFF',
        '-DCMAKE_BUILD_TYPE=Release',
        # f'-DPYTHON_DIST_DIR={install_prefix}',
        '-DSCIKIT_BUILD=ON'
    ],
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C++",
        "Operating System :: OS Independent",
    ],
    python_requires=">=3.6",
    zip_safe=False,
)
