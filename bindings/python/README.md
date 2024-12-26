# Compress Utils - Python API

`compress-utils` aims to simplify data compression by offering a unified interface for various algorithms and languages, while maintaining best-in-class performance.

These docs cover the Python bindings of the `compress-utils` library.

The API is simple and universal across all [available algorithms](#available-algorithms). In Python, there are two flavors:

- [Object-Oriented (OOP) API](#oop-api)
- [Functional API](#functional-api)

## Table of Contents

- [Installation](#installation)
- [Available Algorithms](#available-algorithms)
- [OOP API](#oop-api)
    - [Setup](#setup)
        - [Imports](#imports)
        - [Selecting an Algorithm](#selecting-an-algorithm)
        - [Creating a Compressor Object](#creating-a-compressor-object)
    - [Compression](#compression)
        - [Compressing Data](#compressing-data)
    - [Decompression](#decompression)
        - [Decompressing Data](#decompressing-data)
    - [Functional API](#functional-api)
    - [Setup](#setup-1)
        - [Imports](#imports-1)
        - [Selecting an Algorithm](#selecting-an-algorithm-1)
    - [Compression](#compression-1)
        - [Compressing Data](#compressing-data-1)
    - [Decompression](#decompression-1)
        - [Decompressing Data](#decompressing-data-1)
- [Examples](#examples)
- [Notes](#notes)
- [License](#license)

## Installation

To install the Python bindings, use `pip`:

```bash
# TBD
pip install compress_utils
```

## Available Algorithms

The following compression algorithms are available (depending on your build configuration):

- **brotli**
- **lzma**
- **xz**
- **zlib**
- **zstd**

To check which algorithms are available in your installation, you can list the members of the `Algorithm` enum:

```python
from compress_utils import Algorithm

print(list(Algorithm))
```

## OOP API

### Setup

#### Imports

To use the OOP API, import the `compressor` class and optionally the `Algorithm` enum from the `compress_utils` module:

```python
from compress_utils import compressor, Algorithm
```

#### Selecting an Algorithm

Select a compression algorithm using the `Algorithm` enum or by specifying the algorithm name as a string:

```python
# Using the Algorithm enum
algorithm = Algorithm.zstd

# Or using a string (case-insensitive)
algorithm = 'zstd'
```

#### Creating a Compressor Object

Create a `compressor` object by passing the selected algorithm:

```python
# Create compressor object with the selected algorithm
comp = compressor(algorithm)
```

### Compression

#### Compressing Data

To compress data, use the `compress` method. The data can be any bytes-like object (e.g., `bytes`, `bytearray`):

```python
# Data to compress
data = b'Hello, world!'

# Compress data
compressed_data = comp.compress(data)
```

You can also specify a compression level (typically between 1 and 10):

```python
# Compress data with a compression level (e.g., level 5)
level = 5
compressed_data = comp.compress(data, level)
```

### Decompression

#### Decompressing Data

To decompress data, use the `decompress` method:

```python
# Decompress data
decompressed_data = comp.decompress(compressed_data)
```

The `decompressed_data` will be a `bytes` object containing the original data.

## Functional API

### Setup

#### Imports

To use the functional API, import the `compress` and `decompress` functions and optionally, the `Algorithm` enum:

```python
from compress_utils import compress, decompress, Algorithm
```

#### Selecting an Algorithm

Select the algorithm similarly as before:

```python
# Using the Algorithm enum
algorithm = Algorithm.zstd

# Or using a string
algorithm = 'zstd'
```

### Compression

#### Compressing Data

To compress data using the functional API:

```python
# Data to compress
data = b'Hello, world!'

# Compress data
compressed_data = compress(data, algorithm)
```

You can specify a compression level:

```python
# Compress data with a compression level
level = 5
compressed_data = compress(data, algorithm, level)
```

### Decompression

#### Decompressing Data

To decompress data using the functional API:

```python
# Decompress data
decompressed_data = decompress(compressed_data, algorithm)
```

## Examples

### Listing Available Algorithms

```python
from compress_utils import Algorithm

# List available algorithms
for alg in Algorithm:
    print(alg)
```

**Output:**

```
Algorithm.brotli
Algorithm.lzma
Algorithm.xz
Algorithm.zlib
Algorithm.zstd
```

*(The actual output will depend on which algorithms are included in your build.)*

### Example Using OOP API

```python
from compress_utils import compressor, Algorithm

# Select algorithm
algorithm = Algorithm.zstd

# Create compressor object
comp = compressor(algorithm)

# Data to compress
data = b'This is some data to compress.'

# Compress data
compressed_data = comp.compress(data)

# Decompress data
decompressed_data = comp.decompress(compressed_data)

# Verify that decompressed data matches the original
assert decompressed_data == data
```

### Example Using Functional API

```python
from compress_utils import compress, decompress, Algorithm

# Select algorithm
algorithm = 'zstd'  # or Algorithm.zstd

# Data to compress
data = b'This is some data to compress.'

# Compress data
compressed_data = compress(data, algorithm)

# Decompress data
decompressed_data = decompress(compressed_data, algorithm)

# Verify that decompressed data matches the original
assert decompressed_data == data
```

## Notes

- **Compression Levels**: The `level` parameter controls the compression level. Higher levels typically result in better compression ratios but slower performance. The valid range depends on the algorithm but generally ranges from 1 to 10.

- **Data Types**: The functions expect data to be a bytes-like object (`bytes`, `bytearray`, or any object that implements the buffer protocol). The compressed and decompressed data are returned as `bytes`.

- **Algorithm Specification**: The algorithm can be specified as a string (case-insensitive) or using the `Algorithm` enum. If an unknown algorithm is provided, an `InvalidArgument` exception will be raised.

- **Error Handling**: If compression or decompression fails, an exception will be raised. Ensure you handle exceptions appropriately in your application.