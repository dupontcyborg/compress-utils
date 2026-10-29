//! # compress-utils
//!
//! A unified, safe Rust interface to eight compression algorithms — Zstandard,
//! Brotli, zlib, gzip, bzip2, LZ4, XZ/LZMA and Snappy — over a single C core.
//! Every algorithm speaks the same API; switching codecs is a one-enum change.
//! Raw DEFLATE, raw LZ4 blocks and raw LZO1X blocks (decompression only) are
//! also available; raw blocks don't record their size, so decode them with
//! [`decompress_with_capacity`].
//!
//! ```no_run
//! use compress_utils::{compress, decompress, Algorithm};
//!
//! let data = b"the quick brown fox jumps over the lazy dog";
//! let packed = compress(Algorithm::Zstd, data, 5)?;
//! let restored = decompress(Algorithm::Zstd, &packed)?;
//! assert_eq!(&restored, data);
//! # Ok::<(), compress_utils::Error>(())
//! ```
//!
//! For large or incremental data, use the streaming adapters [`Compressor`]
//! (an [`std::io::Write`]) and [`Decompressor`] (an [`std::io::Read`]) in the
//! [`stream`] module.

use std::ffi::CStr;
use std::fmt;

mod ffi;
pub mod stream;

pub use stream::{Compressor, Decompressor, IntoPartsError};

/// A sensible middle-of-the-road compression level, matching the C++, Python
/// and Go bindings. Levels run 1 (fastest) ..= 10 (smallest).
pub const DEFAULT_LEVEL: i32 = 5;

/// A compression algorithm. Values match the `CU_ALGO_*` constants in the C ABI.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum Algorithm {
    /// Zstandard — frames carry the content size.
    Zstd,
    /// Brotli — raw Brotli stream.
    Brotli,
    /// zlib — DEFLATE with the zlib wrapper (RFC 1950).
    Zlib,
    /// bzip2.
    Bz2,
    /// LZ4 — LZ4 frame format (`.lz4`).
    Lz4,
    /// XZ — `.xz` stream with CRC64.
    Xz,
    /// Alias for [`Algorithm::Xz`]; produces `.xz` frames.
    Lzma,
    /// Snappy — raw Snappy block format.
    Snappy,
    /// gzip — DEFLATE with the gzip wrapper (RFC 1952).
    Gzip,
    /// Raw RFC 1951 DEFLATE.
    Deflate,
    /// Raw LZ4 block, without framing or a size prefix. Supports incremental decoding.
    Lz4Raw,
    /// Raw LZO1X block. Decompression only.
    Lzo,
}

impl Algorithm {
    fn to_raw(self) -> std::os::raw::c_int {
        match self {
            Algorithm::Zstd => ffi::CU_ALGO_ZSTD,
            Algorithm::Brotli => ffi::CU_ALGO_BROTLI,
            Algorithm::Zlib => ffi::CU_ALGO_ZLIB,
            Algorithm::Bz2 => ffi::CU_ALGO_BZ2,
            Algorithm::Lz4 => ffi::CU_ALGO_LZ4,
            Algorithm::Xz => ffi::CU_ALGO_XZ,
            Algorithm::Lzma => ffi::CU_ALGO_LZMA,
            Algorithm::Snappy => ffi::CU_ALGO_SNAPPY,
            Algorithm::Gzip => ffi::CU_ALGO_GZIP,
            Algorithm::Deflate => ffi::CU_ALGO_DEFLATE,
            Algorithm::Lz4Raw => ffi::CU_ALGO_LZ4_RAW,
            Algorithm::Lzo => ffi::CU_ALGO_LZO,
        }
    }

    /// The lowercase canonical name (`"zstd"`, `"brotli"`, ...).
    pub fn name(self) -> &'static str {
        // SAFETY: cu_algorithm_name returns a static NUL-terminated string or
        // NULL; the enum values are always valid, so NULL never occurs here.
        unsafe {
            let p = ffi::cu_algorithm_name(self.to_raw());
            if p.is_null() {
                ""
            } else {
                CStr::from_ptr(p).to_str().unwrap_or("")
            }
        }
    }

    /// Whether this algorithm was compiled into the linked library.
    pub fn available(self) -> bool {
        // SAFETY: pure query over a valid enum value.
        unsafe { ffi::cu_algorithm_available(self.to_raw()) != 0 }
    }

    /// The maximum compressed size for `in_len` input bytes — a safe upper
    /// bound for sizing a one-shot output buffer.
    pub fn compress_bound(self, in_len: usize) -> usize {
        // SAFETY: pure query over a valid enum value.
        unsafe { ffi::cu_compress_bound(in_len, self.to_raw()) }
    }
}

impl fmt::Display for Algorithm {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

/// A C ABI status code, carried on [`Error`] so callers can match on the exact
/// failure. Values match the `CU_ERR_*` constants.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum Status {
    InvalidArg,
    InvalidLevel,
    BufTooSmall,
    SizeUnknown,
    UnsupportedAlgo,
    Compression,
    Decompression,
    Truncated,
    SizeLimit,
    StreamFinished,
    StreamState,
    Oom,
    Internal,
    /// A code not recognised by this binding (carries the raw value).
    Other(i32),
}

impl Status {
    // `c_int` is i32 on every current Rust target, so clippy sees the cast as
    // redundant — but it is a real narrowing on any target where it isn't.
    #[allow(clippy::unnecessary_cast)]
    fn from_raw(code: std::os::raw::c_int) -> Status {
        match code {
            ffi::CU_ERR_INVALID_ARG => Status::InvalidArg,
            ffi::CU_ERR_INVALID_LEVEL => Status::InvalidLevel,
            ffi::CU_ERR_BUF_TOO_SMALL => Status::BufTooSmall,
            ffi::CU_ERR_SIZE_UNKNOWN => Status::SizeUnknown,
            ffi::CU_ERR_UNSUPPORTED_ALGO => Status::UnsupportedAlgo,
            ffi::CU_ERR_COMPRESSION => Status::Compression,
            ffi::CU_ERR_DECOMPRESSION => Status::Decompression,
            ffi::CU_ERR_TRUNCATED => Status::Truncated,
            ffi::CU_ERR_SIZE_LIMIT => Status::SizeLimit,
            ffi::CU_ERR_STREAM_FINISHED => Status::StreamFinished,
            ffi::CU_ERR_STREAM_STATE => Status::StreamState,
            ffi::CU_ERR_OOM => Status::Oom,
            ffi::CU_ERR_INTERNAL => Status::Internal,
            other => Status::Other(other as i32),
        }
    }
}

/// An error returned by the library: a [`Status`] code plus the human-readable
/// message the C core reported.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    /// The status code, for programmatic matching.
    pub code: Status,
    /// The human-readable message (from `cu_strerror` + `cu_last_error`).
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for Error {}

/// Build an [`Error`] from a non-OK status. Reads `cu_last_error()`, which is
/// thread-local, so this must run on the thread that made the failing call —
/// true for every call site here (all synchronous).
fn error_from(code: std::os::raw::c_int) -> Error {
    // SAFETY: both return static / thread-local NUL-terminated strings.
    let mut message = unsafe {
        let p = ffi::cu_strerror(code);
        CStr::from_ptr(p).to_string_lossy().into_owned()
    };
    unsafe {
        let d = ffi::cu_last_error();
        if !d.is_null() {
            let detail = CStr::from_ptr(d).to_string_lossy();
            if !detail.is_empty() {
                message.push_str(": ");
                message.push_str(&detail);
            }
        }
    }
    Error {
        code: Status::from_raw(code),
        message,
    }
}

/// Convert a raw status into `Result<(), Error>`.
fn check(code: std::os::raw::c_int) -> Result<(), Error> {
    if code == ffi::CU_OK {
        Ok(())
    } else {
        Err(error_from(code))
    }
}

/// The linked library version string (`"MAJOR.MINOR.PATCH"`).
pub fn version() -> &'static str {
    // SAFETY: cu_version returns a static NUL-terminated string.
    unsafe { CStr::from_ptr(ffi::cu_version()).to_str().unwrap_or("") }
}

/// Set a global cap on the output size [`decompress`] and
/// [`decompress_with_capacity`] will accept, for every algorithm. Defaults to
/// 1 GiB; `0` disables the cap. A [`Decompressor`] has no limit unless set
/// with [`Decompressor::set_max_output`].
pub fn set_max_decompressed_size(bytes: usize) {
    // SAFETY: plain scalar setter, thread-safe per the C ABI contract.
    unsafe { ffi::cu_set_max_decompressed_size(bytes) }
}

/// Compress `data` with `algo` at `level` (1 fastest ..= 10 smallest).
pub fn compress(algo: Algorithm, data: &[u8], level: i32) -> Result<Vec<u8>, Error> {
    let bound = algo.compress_bound(data.len());
    let mut out = vec![0u8; bound];
    let mut out_len = out.len();
    // SAFETY: `data`/`out` pointers are valid for their lengths; empty slices
    // yield non-null dangling pointers (Rust never hands out a null slice ptr),
    // so no NULL-source special-casing is needed. out_len is in/out.
    let st = unsafe {
        ffi::cu_compress(
            algo.to_raw(),
            data.as_ptr(),
            data.len(),
            out.as_mut_ptr(),
            &mut out_len,
            level,
        )
    };
    check(st)?;
    out.truncate(out_len);
    Ok(out)
}

/// Decompress `data` produced by `algo`. When the wire format records the
/// decompressed size it is decoded straight into one right-sized allocation; otherwise the C core decodes into a buffer it grows (bounded by the same cap as the sized path) and the result is copied out of it once. Raw LZ4 can use this growing decode without a capacity hint.
pub fn decompress(algo: Algorithm, data: &[u8]) -> Result<Vec<u8>, Error> {
    let mut hint: usize = 0;
    // SAFETY: valid slice, valid out pointer.
    let st = unsafe {
        ffi::cu_decompress_size_hint(algo.to_raw(), data.as_ptr(), data.len(), &mut hint)
    };
    match st {
        ffi::CU_OK => {
            // Never blindly allocate the reported size: a malformed header can claim a bogus, enormous length (some codecs' size_hint does not fully validate), and an infallible allocation would abort the process on OOM. If it can't be reserved, the core's growing decode takes over: it allocates only as real output appears, and rejects the bad input.
            let mut out: Vec<u8> = Vec::new();
            if out.try_reserve_exact(hint).is_ok() {
                match decode_into_spare(algo, data, &mut out, hint) {
                    ffi::CU_OK => return Ok(out),
                    // A hint that was short of the real size.
                    ffi::CU_ERR_SIZE_UNKNOWN | ffi::CU_ERR_BUF_TOO_SMALL => {}
                    other => return Err(error_from(other)),
                }
            }
        }
        ffi::CU_ERR_SIZE_UNKNOWN => {}
        other => return Err(error_from(other)),
    }
    decompress_alloc(algo, data)
}

/// Decompress `data` into at most `capacity` bytes. The capacity is an upper
/// bound on the output, allocated up front; the result holds only the bytes
/// written. Skips the size probe and the growing decode for every algorithm.
pub fn decompress_with_capacity(
    algo: Algorithm,
    data: &[u8],
    capacity: usize,
) -> Result<Vec<u8>, Error> {
    let mut out: Vec<u8> = Vec::new();
    if out.try_reserve_exact(capacity).is_err() {
        return Err(oom(capacity));
    }
    check(decode_into_spare(algo, data, &mut out, capacity))?;
    Ok(out)
}

/// Decodes `data` into the first `cap` bytes of `out`'s spare capacity, which are left uninitialized until the codec writes them, and sets `out`'s length to the bytes written on success. Returns the raw status; `out` stays empty on failure. `out` must be empty with at least `cap` bytes of capacity.
fn decode_into_spare(
    algo: Algorithm,
    data: &[u8],
    out: &mut Vec<u8>,
    cap: usize,
) -> std::os::raw::c_int {
    debug_assert!(out.is_empty() && out.capacity() >= cap);
    let mut out_len = cap;
    // SAFETY: `data` is a valid slice; the spare capacity is valid for `cap` bytes of writes (an empty one yields a non-null dangling pointer, which the C side accepts for zero capacity); out_len is in/out.
    let st = unsafe {
        ffi::cu_decompress(
            algo.to_raw(),
            data.as_ptr(),
            data.len(),
            out.spare_capacity_mut().as_mut_ptr().cast::<u8>(),
            &mut out_len,
        )
    };
    if st == ffi::CU_OK {
        assert!(
            out_len <= cap,
            "compress-utils: decoder reported more output than the buffer holds"
        );
        // SAFETY: the codec initialized the first out_len <= capacity bytes.
        unsafe { out.set_len(out_len) };
    }
    st
}

/// Decodes into a buffer the C core allocates and grows (for a format that doesn't record its size), then copies it once into a `Vec`.
fn decompress_alloc(algo: Algorithm, data: &[u8]) -> Result<Vec<u8>, Error> {
    let mut buf: *mut u8 = std::ptr::null_mut();
    let mut len: usize = 0;
    // SAFETY: valid slice; out pointers are valid. On success the core hands over a malloc'd buffer that is released with cu_buffer_free below.
    let st = unsafe {
        ffi::cu_decompress_alloc(algo.to_raw(), data.as_ptr(), data.len(), &mut buf, &mut len)
    };
    check(st)?;
    let mut out: Vec<u8> = Vec::new();
    let copied = out.try_reserve_exact(len).is_ok();
    if copied {
        // SAFETY: on success `buf` holds `len` initialized bytes.
        out.extend_from_slice(unsafe { std::slice::from_raw_parts(buf, len) });
    }
    // SAFETY: `buf` came from cu_decompress_alloc and isn't used after this.
    unsafe { ffi::cu_buffer_free(buf.cast()) };
    if copied {
        Ok(out)
    } else {
        Err(oom(len))
    }
}

fn oom(bytes: usize) -> Error {
    Error {
        code: Status::Oom,
        message: format!("compress-utils: cannot allocate {bytes} bytes of output"),
    }
}

// Re-exported for the streaming module to construct/report errors uniformly.
pub(crate) use self::check as check_status;
pub(crate) use self::error_from as make_error;
