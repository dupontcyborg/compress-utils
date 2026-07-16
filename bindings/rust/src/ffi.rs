//! Raw FFI declarations for the compress-utils C ABI.
//!
//! Hand-written (not bindgen-generated) because the C surface in
//! `include/compress_utils.h` is small and stable, and hand-writing keeps
//! `bindgen`/`libclang` off every consumer's build. This module is the single
//! place that mirrors the header; keep the two in sync. Everything here is
//! `unsafe` to call — the safe wrappers live in `lib.rs` / `stream.rs`.

use std::os::raw::{c_char, c_int};

// cu_algorithm_t — values match the CU_ALGO_* enumerators.
pub const CU_ALGO_ZSTD: c_int = 0;
pub const CU_ALGO_BROTLI: c_int = 1;
pub const CU_ALGO_ZLIB: c_int = 2;
pub const CU_ALGO_BZ2: c_int = 3;
pub const CU_ALGO_LZ4: c_int = 4;
pub const CU_ALGO_XZ: c_int = 5;
pub const CU_ALGO_LZMA: c_int = 6;
pub const CU_ALGO_SNAPPY: c_int = 7;
pub const CU_ALGO_GZIP: c_int = 8;

// cu_status_t
pub const CU_OK: c_int = 0;
pub const CU_ERR_INVALID_ARG: c_int = 1;
pub const CU_ERR_INVALID_LEVEL: c_int = 2;
pub const CU_ERR_BUF_TOO_SMALL: c_int = 3;
pub const CU_ERR_SIZE_UNKNOWN: c_int = 4;
pub const CU_ERR_UNSUPPORTED_ALGO: c_int = 5;
pub const CU_ERR_COMPRESSION: c_int = 6;
pub const CU_ERR_DECOMPRESSION: c_int = 7;
pub const CU_ERR_TRUNCATED: c_int = 8;
pub const CU_ERR_SIZE_LIMIT: c_int = 9;
pub const CU_ERR_STREAM_FINISHED: c_int = 10;
pub const CU_ERR_STREAM_STATE: c_int = 11;
pub const CU_ERR_OOM: c_int = 12;
pub const CU_ERR_INTERNAL: c_int = 13;

// Opaque stream handles. The C side never exposes the layout; we only ever hold
// pointers to them, so zero-field structs are the idiomatic Rust representation.
#[repr(C)]
pub struct cu_compress_stream {
    _private: [u8; 0],
}
#[repr(C)]
pub struct cu_decompress_stream {
    _private: [u8; 0],
}

extern "C" {
    pub fn cu_version() -> *const c_char;

    pub fn cu_algorithm_name(algo: c_int) -> *const c_char;
    pub fn cu_algorithm_available(algo: c_int) -> c_int;

    pub fn cu_strerror(code: c_int) -> *const c_char;
    pub fn cu_last_error() -> *const c_char;
    // Part of the ABI; not surfaced by the safe API yet.
    #[allow(dead_code)]
    pub fn cu_clear_last_error();

    pub fn cu_compress_bound(in_len: usize, algo: c_int) -> usize;

    pub fn cu_compress(
        algo: c_int,
        input: *const u8,
        in_len: usize,
        out: *mut u8,
        out_len: *mut usize,
        level: c_int,
    ) -> c_int;

    pub fn cu_decompress(
        algo: c_int,
        input: *const u8,
        in_len: usize,
        out: *mut u8,
        out_len: *mut usize,
    ) -> c_int;

    pub fn cu_decompress_size_hint(
        algo: c_int,
        input: *const u8,
        in_len: usize,
        out_size: *mut usize,
    ) -> c_int;

    pub fn cu_set_max_decompressed_size(bytes: usize);

    pub fn cu_compress_stream_create(
        algo: c_int,
        level: c_int,
        out_stream: *mut *mut cu_compress_stream,
    ) -> c_int;
    pub fn cu_compress_stream_write(
        stream: *mut cu_compress_stream,
        input: *const u8,
        in_len: usize,
        out: *mut u8,
        out_len: *mut usize,
    ) -> c_int;
    pub fn cu_compress_stream_finish(
        stream: *mut cu_compress_stream,
        out: *mut u8,
        out_len: *mut usize,
    ) -> c_int;
    pub fn cu_compress_stream_destroy(stream: *mut cu_compress_stream);

    pub fn cu_decompress_stream_create(
        algo: c_int,
        out_stream: *mut *mut cu_decompress_stream,
    ) -> c_int;
    pub fn cu_decompress_stream_write(
        stream: *mut cu_decompress_stream,
        input: *const u8,
        in_len: usize,
        out: *mut u8,
        out_len: *mut usize,
    ) -> c_int;
    pub fn cu_decompress_stream_finish(
        stream: *mut cu_decompress_stream,
        out: *mut u8,
        out_len: *mut usize,
    ) -> c_int;
    pub fn cu_decompress_stream_destroy(stream: *mut cu_decompress_stream);
}
