from skbuild import setup

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
        '-DSCIKIT_BUILD=ON'
    ],
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C++",
        "Operating System :: OS Independent",
    ],
    build_options=['-j8'],
    python_requires=">=3.6",
    zip_safe=False,
)
