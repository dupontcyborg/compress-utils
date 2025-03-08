# compress-utils

[![PyPI version](https://badge.fury.io/py/compress-utils.svg)](https://badge.fury.io/py/compress-utils)
[![Python Versions](https://img.shields.io/pypi/pyversions/compress-utils.svg)](https://pypi.org/project/compress-utils/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A unified, high-performance interface for multiple compression algorithms across programming languages.

## Features

- **Multiple Algorithms**: Integrated support for Brotli, LZMA, XZ, zlib, and Zstandard
- **Consistent API**: Same interface across all supported algorithms
- **Cross-Language**: Core C++ library with language bindings for Python (and more to come)
- **Performance Focused**: Built to minimize overhead over native compression libraries

## Installation

```bash
pip install compress-utils
```

## Quick Start

### Object-Oriented API

```python
from compress_utils import compressor, Algorithm

# Create a compressor with your algorithm of choice
comp = compressor("zstd")  # or 'brotli', 'xz', etc.

# Compress data with optional compression level (1-10)
compressed = comp.compress(b"Hello, world!", level=3)

# Decompress data
original = comp.decompress(compressed)
```

### Functional API

```python
from compress_utils import compress, decompress

# Compress and decompress directly
compressed = compress(b"Hello, world!", "zstd", level=3)
original = decompress(compressed, "zstd")
```

## Available Algorithms

The following algorithms are supported (availability depends on build configuration):

- **Brotli** - Google's compression algorithm optimized for the web
- **LZMA/XZ** - High compression ratio algorithms
- **zlib** - Widely used general-purpose compression
- **Zstandard** - Fast compression algorithm with high ratios

## Documentation

For detailed API documentation and advanced usage, see the [full documentation](https://github.com/dupontcyborg/compress-utils/blob/main/bindings/python/API.md).

## Contributing

Contributions are welcome! Check out the [issues page](https://github.com/dupontcyborg/compress-utils/issues) for open tasks or submit your own ideas.

## License

This project is licensed under the MIT License - see the LICENSE file for details.