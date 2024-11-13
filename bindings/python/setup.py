from skbuild import setup

setup(
    name="compress_utils_py",
    version="0.4.0",
    description="Simple & high-performance compression utilities for Python",
    author="Nicolas Dupont",
    license="MIT",
    packages=['compress_utils_py'],
    package_dir={'compress_utils_py': 'bindings/python'},
    package_data={
        'compress_utils_py': [
            'compress_utils_py*.so',    # For Linux shared libraries
            'compress_utils_py*.dylib', # For macOS shared libraries
            'compress_utils_py*.dll',   # For Windows shared libraries
            'README.md',
            'LICENSE'
        ]
    },
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
    python_requires=">=3.6",
    zip_safe=False,
)
