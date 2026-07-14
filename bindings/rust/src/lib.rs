//! # compress-utils
//!
//! A unified, safe Rust interface to eight compression algorithms — Zstandard,
//! Brotli, zlib, gzip, bzip2, LZ4, XZ/LZMA and Snappy — over a single C core.
//! Every algorithm speaks the same API; switching codecs is a one-enum change.
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
use std::sync::atomic::{AtomicUsize, Ordering};

mod ffi;
pub mod stream;

pub use stream::{Compressor, Decompressor};

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

/// Mirror of the C core's decompressed-size cap (default 1 GiB, matching the C
/// default). `usize::MAX` means "disabled". Consulted by [`decompress`] when it
/// falls back to streaming for size-unknown formats — that path bypasses
/// `cu_decompress`, so we re-apply the cap here to keep the decompress-to-memory
/// API safe against decompression bombs (e.g. malformed xz that liblzma's
/// legacy `.lzma` auto-detection expands without bound).
static MAX_DECOMPRESSED: AtomicUsize = AtomicUsize::new(1024 * 1024 * 1024);

/// Set a global cap on the output size one-shot [`decompress`] will accept.
/// Defaults to 1 GiB; `0` disables the cap. Does not affect the streaming
/// [`Decompressor`], which should enforce its own limit.
pub fn set_max_decompressed_size(bytes: usize) {
    // SAFETY: plain scalar setter, thread-safe per the C ABI contract.
    unsafe { ffi::cu_set_max_decompressed_size(bytes) }
    MAX_DECOMPRESSED.store(
        if bytes == 0 { usize::MAX } else { bytes },
        Ordering::Relaxed,
    );
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
/// decompressed size it is recovered in a single pass; otherwise this falls
/// back to the streaming decoder (as the C++/Python/Go bindings do).
pub fn decompress(algo: Algorithm, data: &[u8]) -> Result<Vec<u8>, Error> {
    match decompress_one_shot(algo, data)? {
        Some(out) => Ok(out),
        // Wire format does not carry the size (zlib, gzip, brotli, bz2, raw
        // lz4, snappy): stream-decompress the whole buffer, bounded by the same
        // cap the C one-shot path enforces so a malformed bomb can't run us out
        // of memory here.
        None => {
            use std::io::Read;
            let mut r = Decompressor::new(data, algo)?;
            let mut out = Vec::new();
            let cap = MAX_DECOMPRESSED.load(Ordering::Relaxed);
            if cap == usize::MAX {
                // Cap disabled by the caller — honour their choice, unbounded.
                r.read_to_end(&mut out).map_err(stream::io_to_error)?;
            } else {
                // Read at most cap+1 bytes; more than cap means it exceeded.
                let limit = (cap as u64).saturating_add(1);
                r.by_ref()
                    .take(limit)
                    .read_to_end(&mut out)
                    .map_err(stream::io_to_error)?;
                if out.len() > cap {
                    return Err(Error {
                        code: Status::SizeLimit,
                        message: format!(
                            "compress-utils: decompressed size exceeds cap of {cap} bytes"
                        ),
                    });
                }
            }
            Ok(out)
        }
    }
}

/// Attempt the size-hint + one-shot path. `Ok(None)` means the caller should
/// fall back to streaming (size not recoverable from the wire format).
fn decompress_one_shot(algo: Algorithm, data: &[u8]) -> Result<Option<Vec<u8>>, Error> {
    let mut hint: usize = 0;
    // SAFETY: valid slice, valid out pointer.
    let st = unsafe {
        ffi::cu_decompress_size_hint(algo.to_raw(), data.as_ptr(), data.len(), &mut hint)
    };
    match st {
        ffi::CU_OK => {
            // Never blindly allocate the reported size: a malformed header can
            // claim a bogus, enormous length (some codecs' size_hint does not
            // fully validate), and `vec![0u8; hint]` would abort the process on
            // OOM. Try to reserve; if that fails, fall back to streaming, which
            // decodes incrementally and rejects the bad input via the
            // no-progress guard.
            let mut out: Vec<u8> = Vec::new();
            if out.try_reserve_exact(hint).is_err() {
                return Ok(None);
            }
            out.resize(hint, 0);
            let mut out_len = out.len();
            // SAFETY: as above; out sized to the reported hint.
            let st2 = unsafe {
                ffi::cu_decompress(
                    algo.to_raw(),
                    data.as_ptr(),
                    data.len(),
                    out.as_mut_ptr(),
                    &mut out_len,
                )
            };
            match st2 {
                ffi::CU_OK => {
                    out.truncate(out_len);
                    Ok(Some(out))
                }
                ffi::CU_ERR_SIZE_UNKNOWN | ffi::CU_ERR_BUF_TOO_SMALL => Ok(None),
                other => Err(error_from(other)),
            }
        }
        ffi::CU_ERR_SIZE_UNKNOWN => Ok(None),
        other => Err(error_from(other)),
    }
}

// Re-exported for the streaming module to construct/report errors uniformly.
pub(crate) use self::check as check_status;
pub(crate) use self::error_from as make_error;
