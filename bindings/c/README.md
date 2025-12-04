# Compress Utils - C API

`compress-utils` aims to simplify data compression by offering a unified interface for various algorithms and languages, while maintaining best-in-class performance. 

These docs cover the C binding. The API is simple and universal across all [available algorithms](/README.md#built-in-compression-algorithms).

## Table of Contents

- [Usage](#usage)
    - [Setup](#setup)
        - [Includes](#includes)
        - [Selecting Algorithm](#selecting-algorithm)
    - [One-Shot Compression](#one-shot-compression)
    - [One-Shot Decompression](#one-shot-decompression)
    - [Streaming API](#streaming-api)
        - [Streaming Compression](#streaming-compression)
        - [Streaming Decompression](#streaming-decompression)

## Usage

### Setup

#### Includes

The entire `compress-utils` library is available through a single header:

```c
#include "compress_utils.h"
```

### Selecting Algorithm

Before calling `compress()` or `decompress()`, you must select a compression algorithm from the `Algorithms` enum:

```c
// Select algorithm
Algorithm algorithm = ZSTD;
```

### One-Shot Compression

To compress data from a `uint8_t*` pointer in a single operation, you can call `compress_data()` via:

```c
// Compress data
uint8_t* comp_data = NULL;
int level = 3;  // Compression level: 1 (fastest) to 10 (smallest)
int64_t comp_size = compress_data(data, data_size, &comp_data, algorithm, level);

// Check if compression succeeded
if (comp_size == -1) {
    // Handle compression error
}

// Clean up
free(comp_data);
```

Note that `compress_data()` will allocate memory at the `comp_data` pointer, so be sure to free that memory when you've finished using it.

### One-Shot Decompression

To decompress data from a `uint8_t*` pointer in a single operation, you can call `decompress_data()` via:

```c
// Decompress data
uint8_t* decomp_data = NULL;
int64_t decomp_size = decompress_data(comp_data, comp_size, &decomp_data, algorithm);

// Check if decompression succeeded
if (decomp_size == -1) {
    // Handle decompression error
}

free(decomp_data);
```

Note that `decompress_data()` will allocate memory at the `decomp_data` pointer, so be sure to free that memory when you've finished using it.

### Streaming API

For processing large data in chunks or when data arrives incrementally, use the streaming API. This is ideal for network streams, large files, or real-time data processing.

#### Streaming Compression

```c
// Create a compression stream
int level = 3;  // Compression level: 1 (fastest) to 10 (smallest)
CompressStream* stream = compress_stream_create(algorithm, level);

// Compress data in chunks
uint8_t* output_buffer = NULL;
size_t total_compressed = 0;

for (size_t i = 0; i < data_size; i += chunk_size) {
    size_t this_chunk = (i + chunk_size > data_size) ? (data_size - i) : chunk_size;

    // Compress chunk
    uint8_t* chunk_output = NULL;
    int64_t chunk_output_size = compress_stream_compress(stream, data + i, this_chunk, &chunk_output);

    if (chunk_output_size > 0) {
        // Append chunk_output to your output buffer
        // Remember to free chunk_output when done
        free(chunk_output);
    }
}

// Finalize compression (important!)
uint8_t* final_output = NULL;
int64_t final_size = compress_stream_finish(stream, &final_output);
if (final_size > 0) {
    // Append final_output to your output buffer
    free(final_output);
}

// Clean up
compress_stream_destroy(stream);
```

#### Streaming Decompression

```c
// Create a decompression stream
DecompressStream* stream = decompress_stream_create(algorithm);

// Decompress data in chunks
for (size_t i = 0; i < compressed_size; i += chunk_size) {
    size_t this_chunk = (i + chunk_size > compressed_size) ? (compressed_size - i) : chunk_size;

    // Decompress chunk
    uint8_t* chunk_output = NULL;
    int64_t chunk_output_size = decompress_stream_decompress(stream, compressed_data + i, this_chunk, &chunk_output);

    if (chunk_output_size > 0) {
        // Append chunk_output to your output buffer
        free(chunk_output);
    }
}

// Finalize decompression (important!)
uint8_t* final_output = NULL;
int64_t final_size = decompress_stream_finish(stream, &final_output);
if (final_size > 0) {
    // Append final_output to your output buffer
    free(final_output);
}

// Clean up
decompress_stream_destroy(stream);
```

**Key points about streaming:**
- Always call `compress_stream_finish()` or `decompress_stream_finish()` when done to flush any remaining data
- Check `compress_stream_is_finished()` or `decompress_stream_is_finished()` to verify stream state
- Use `compress_stream_get_algorithm()` or `decompress_stream_get_algorithm()` to retrieve the algorithm
- Remember to free all output buffers and destroy streams when finished