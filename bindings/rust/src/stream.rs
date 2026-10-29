//! Streaming compression and decompression as standard [`std::io`] adapters.
//!
//! [`Compressor`] is a [`Write`] that compresses everything written to it and
//! forwards the result to an inner sink. [`Decompressor`] is a [`Read`] that
//! decompresses from an inner source on demand. Both drive the C ABI's
//! fill-buffer / `BUF_TOO_SMALL` / drain protocol internally, so callers never
//! see it.
//!
//! Neither type is safe for concurrent use (each owns a non-thread-safe C
//! stream handle).

use std::fmt;
use std::io::{self, Read, Write};

use crate::ffi;
use crate::{Algorithm, Error, Status};

/// Size of the fixed scratch buffer each stream drains through. The protocol
/// works with any size; 64 KiB keeps the FFI call count low without holding
/// much memory. Matches the Go binding.
const SCRATCH: usize = 64 * 1024;

impl From<Error> for io::Error {
    fn from(e: Error) -> io::Error {
        io::Error::new(io::ErrorKind::Other, e)
    }
}

/// A `BUF_TOO_SMALL` status always accompanies a fully-written output buffer;
/// if the codec reports it with zero bytes produced it is stuck (malformed
/// input, or a decoder that can neither consume nor emit) and looping would
/// spin forever / grow memory unbounded. Turn that into a hard error.
fn stuck_error() -> io::Error {
    Error {
        code: Status::Decompression,
        message: "compress-utils: decoder made no progress (malformed or truncated input)"
            .to_string(),
    }
    .into()
}

/// A [`Write`] that compresses into an inner sink.
///
/// You **must** call [`Compressor::finish`] to flush the final frame and get a
/// clean, decodable stream. As a safety net the frame is also finalized on drop
/// (best-effort, errors ignored), but relying on that discards any I/O error —
/// prefer `finish`.
pub struct Compressor<W: Write> {
    sink: Option<W>,
    stream: *mut ffi::cu_compress_stream,
    scratch: Vec<u8>,
    finished: bool,
}

impl<W: Write> Compressor<W> {
    /// Create a compressor writing to `sink` with `algo` at `level`
    /// (1 fastest ..= 10 smallest).
    pub fn new(sink: W, algo: Algorithm, level: i32) -> Result<Self, Error> {
        let mut stream: *mut ffi::cu_compress_stream = std::ptr::null_mut();
        // SAFETY: out_stream points at a local; on CU_OK it is set to a valid
        // handle we own until destroy.
        let st = unsafe { ffi::cu_compress_stream_create(algo.to_raw(), level, &mut stream) };
        crate::check_status(st)?;
        Ok(Compressor {
            sink: Some(sink),
            stream,
            scratch: vec![0u8; SCRATCH],
            finished: false,
        })
    }

    /// Flush the final frame, release the C stream, and return the inner sink.
    pub fn finish(mut self) -> io::Result<W> {
        self.do_finish()?;
        self.destroy();
        // SAFETY: sink is always Some until finish/drop takes it exactly once.
        Ok(self.sink.take().expect("sink present"))
    }

    /// Drain the finalize phase to the sink. Idempotent.
    fn do_finish(&mut self) -> io::Result<()> {
        if self.finished {
            return Ok(());
        }
        loop {
            let mut out_len = self.scratch.len();
            // SAFETY: valid handle + scratch buffer; out_len is in/out.
            let st = unsafe {
                ffi::cu_compress_stream_finish(self.stream, self.scratch.as_mut_ptr(), &mut out_len)
            };
            if out_len > 0 {
                self.sink
                    .as_mut()
                    .expect("sink present")
                    .write_all(&self.scratch[..out_len])?;
            }
            if st == ffi::CU_OK {
                self.finished = true;
                return Ok(());
            }
            if st != ffi::CU_ERR_BUF_TOO_SMALL {
                return Err(crate::make_error(st).into());
            }
            if out_len == 0 {
                return Err(stuck_error());
            }
        }
    }

    fn destroy(&mut self) {
        if !self.stream.is_null() {
            // SAFETY: handle created by create() and not yet destroyed.
            unsafe { ffi::cu_compress_stream_destroy(self.stream) };
            self.stream = std::ptr::null_mut();
        }
    }
}

impl<W: Write> Write for Compressor<W> {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if self.finished {
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "compress-utils: stream is finished",
            ));
        }
        if buf.is_empty() {
            return Ok(0);
        }
        let sink = self.sink.as_mut().expect("sink present");
        let mut first = true;
        loop {
            let mut out_len = self.scratch.len();
            // First call feeds the input; the stream retains any unconsumed
            // tail internally, so subsequent drain calls feed (NULL, 0).
            let (in_ptr, in_len) = if first {
                (buf.as_ptr(), buf.len())
            } else {
                (std::ptr::null(), 0usize)
            };
            first = false;
            // SAFETY: in/out pointers valid for their lengths; out_len in/out.
            let st = unsafe {
                ffi::cu_compress_stream_write(
                    self.stream,
                    in_ptr,
                    in_len,
                    self.scratch.as_mut_ptr(),
                    &mut out_len,
                )
            };
            if out_len > 0 {
                sink.write_all(&self.scratch[..out_len])?;
            }
            if st == ffi::CU_OK {
                return Ok(buf.len());
            }
            if st != ffi::CU_ERR_BUF_TOO_SMALL {
                return Err(crate::make_error(st).into());
            }
            if out_len == 0 {
                return Err(stuck_error());
            }
        }
    }

    /// Flushes the codec, then the sink: everything written so far reaches
    /// the sink in decodable form, and the frame stays open for more writes
    /// (as flate2's `flush` does). Each flush costs a few bytes of output.
    ///
    /// Returns an [`io::ErrorKind::Unsupported`] error for Snappy and Bz2,
    /// which can't flush mid-stream.
    fn flush(&mut self) -> io::Result<()> {
        let Some(sink) = self.sink.as_mut() else {
            return Ok(());
        };
        while !self.finished {
            let mut out_len = self.scratch.len();
            // SAFETY: valid handle + scratch buffer; out_len is in/out.
            let st = unsafe {
                ffi::cu_compress_stream_flush(self.stream, self.scratch.as_mut_ptr(), &mut out_len)
            };
            if out_len > 0 {
                sink.write_all(&self.scratch[..out_len])?;
            }
            match st {
                ffi::CU_OK => break,
                ffi::CU_ERR_BUF_TOO_SMALL if out_len == 0 => return Err(stuck_error()),
                ffi::CU_ERR_BUF_TOO_SMALL => {}
                ffi::CU_ERR_UNSUPPORTED_ALGO => {
                    return Err(io::Error::new(
                        io::ErrorKind::Unsupported,
                        crate::make_error(st),
                    ))
                }
                other => return Err(crate::make_error(other).into()),
            }
        }
        sink.flush()
    }
}

impl<W: Write> Drop for Compressor<W> {
    fn drop(&mut self) {
        // Best-effort finalize so a forgotten finish() still yields a decodable
        // stream; errors are unreportable here, hence finish() is preferred.
        if self.sink.is_some() && !self.finished {
            let _ = self.do_finish();
        }
        self.destroy();
    }
}

/// A [`Read`] that decompresses from an inner source.
///
/// By default concatenated zstd/LZ4 frames, gzip members and xz streams decode
/// as one stream, and the source must end on a frame boundary. A decompressor
/// made with [`Decompressor::single_stream`] instead stops at the end of the
/// first frame and hands back the bytes after it.
pub struct Decompressor<R: Read> {
    /// `None` only once [`Decompressor::into_parts`] has taken it.
    src: Option<R>,
    stream: *mut ffi::cu_decompress_stream,
    in_buf: Vec<u8>,
    scratch: Vec<u8>,
    out: Vec<u8>,
    out_pos: usize,

    input_done: bool,
    draining: bool,
    finished: bool,

    single: bool,
    eof: bool,
    /// Single mode: the bytes read from `src` after the end of the stream.
    unused: Vec<u8>,
    /// An error held back until the output decoded before it has been read.
    pending: Option<io::Error>,
}

impl<R: Read> Decompressor<R> {
    /// Create a decompressor reading a stream produced by `algo` from `src`.
    pub fn new(src: R, algo: Algorithm) -> Result<Self, Error> {
        Self::create(src, algo, 0)
    }

    /// Create a decompressor that stops at the end of the first frame (zstd or
    /// LZ4 frame, gzip member, zlib/DEFLATE/xz/brotli/bzip2 stream, or Snappy
    /// block) instead of continuing into whatever follows it, such as another
    /// message on the same connection.
    ///
    /// `read` returns `Ok(0)` once that frame is fully decoded. It reads `src`
    /// in chunks, so it usually reads past the end: those bytes come back from
    /// [`unused`](Self::unused) or [`into_parts`](Self::into_parts), so the
    /// caller can carry on reading the source:
    ///
    /// ```
    /// use std::io::{Cursor, Read};
    /// use compress_utils::{compress, Algorithm, Decompressor};
    ///
    /// let mut wire = compress(Algorithm::Zstd, b"first", 5).unwrap();
    /// wire.extend_from_slice(b"rest of the source");
    ///
    /// let mut d = Decompressor::single_stream(Cursor::new(wire), Algorithm::Zstd).unwrap();
    /// let mut first = Vec::new();
    /// d.read_to_end(&mut first).unwrap();
    /// assert_eq!(first, b"first");
    ///
    /// let (src, unused) = d.into_parts().unwrap();
    /// let mut rest = Vec::new();
    /// Cursor::new(unused).chain(src).read_to_end(&mut rest).unwrap();
    /// assert_eq!(rest, b"rest of the source");
    /// ```
    ///
    /// Raw LZ4 and LZO have no end marker to stop at and return
    /// [`Status::UnsupportedAlgo`]. Single-mode Snappy produces no output until
    /// its whole block has arrived, and rejects a block declaring more than
    /// [`crate::set_max_decompressed_size`] (as set when the decompressor is
    /// created) with [`Status::SizeLimit`].
    pub fn single_stream(src: R, algo: Algorithm) -> Result<Self, Error> {
        Self::create(src, algo, ffi::CU_DSTREAM_SINGLE)
    }

    fn create(src: R, algo: Algorithm, flags: u32) -> Result<Self, Error> {
        let mut stream: *mut ffi::cu_decompress_stream = std::ptr::null_mut();
        // SAFETY: out_stream points at a local; set to a valid handle on CU_OK.
        let st = unsafe {
            if flags == 0 {
                ffi::cu_decompress_stream_create(algo.to_raw(), &mut stream)
            } else {
                ffi::cu_decompress_stream_create_ex(algo.to_raw(), flags, &mut stream)
            }
        };
        crate::check_status(st)?;
        Ok(Decompressor {
            src: Some(src),
            stream,
            in_buf: vec![0u8; SCRATCH],
            scratch: vec![0u8; SCRATCH],
            out: Vec::new(),
            out_pos: 0,
            input_done: false,
            draining: false,
            finished: false,
            single: flags & ffi::CU_DSTREAM_SINGLE != 0,
            eof: false,
            unused: Vec::new(),
            pending: None,
        })
    }

    /// Limit the total decompressed output to `bytes` (`0`, the default, means
    /// no limit). The `read` that would go past it returns the bytes up to the
    /// limit, then an error with [`Status::SizeLimit`]; the decompressor is
    /// done after that. Use it when decompressing untrusted input, which can
    /// expand to far more than its own size.
    pub fn set_max_output(&mut self, bytes: usize) {
        // SAFETY: `stream` is a live handle owned by `self`; the call only
        // stores the limit.
        unsafe { ffi::cu_decompress_stream_set_max_output(self.stream, bytes) };
    }

    /// Single-stream mode: whether the end of the stream has been decoded.
    /// Output decoded before it may still be waiting to be read; `read`
    /// returns `Ok(0)` once it has been. Always `false` for decompressors made
    /// with [`Decompressor::new`].
    pub fn is_eof(&self) -> bool {
        self.eof
    }

    /// Single-stream mode: the bytes read from the source after the end of the
    /// stream, in order. Empty until [`is_eof`](Self::is_eof), and always for
    /// decompressors made with [`Decompressor::new`].
    pub fn unused(&self) -> &[u8] {
        &self.unused
    }

    /// Returns the source and the bytes already read from it after the end of
    /// the stream ([`unused`](Self::unused)). Reading the unused bytes, then
    /// the source, continues exactly where the compressed stream ended.
    ///
    /// Only once the stream has ended and all of its output has been read
    /// (`read` has returned `Ok(0)`): before that, part of the stream and the
    /// bytes after it may still be held inside the decompressor, and taking
    /// the source would lose them. Otherwise this returns an
    /// [`IntoPartsError`] that hands the decompressor back, so you can keep
    /// reading. A caller that stops at the end of its own data (as `pickle`
    /// stops at its STOP opcode) should read to `Ok(0)` first.
    pub fn into_parts(mut self) -> Result<(R, Vec<u8>), IntoPartsError<R>> {
        if !self.finished || self.out_pos < self.out.len() {
            return Err(IntoPartsError(self));
        }
        let unused = std::mem::take(&mut self.unused);
        let src = self.src.take().expect("source present until into_parts");
        Ok((src, unused))
    }

    /// Single mode: after a call that returned CU_OK, latch eof once the C
    /// stream reports it and keep the input it didn't use. Every byte read from
    /// `src` is passed to the C stream in the same call, so its unused bytes
    /// are all the read-ahead there is.
    fn settle_single(&mut self) {
        if !self.single || self.eof {
            return;
        }
        // SAFETY: valid handle.
        if unsafe { ffi::cu_decompress_stream_eof(self.stream) } == 0 {
            return;
        }
        let mut data: *const u8 = std::ptr::null();
        let mut len = 0usize;
        // SAFETY: valid handle; data/len point at locals. The C buffer stays
        // valid until the next call on the stream, and is copied before then.
        let st = unsafe { ffi::cu_decompress_stream_unused(self.stream, &mut data, &mut len) };
        if st == ffi::CU_OK && len > 0 && !data.is_null() {
            // SAFETY: C guarantees `data` points at `len` readable bytes.
            self.unused
                .extend_from_slice(unsafe { std::slice::from_raw_parts(data, len) });
        }
        self.eof = true;
        self.finished = true;
    }

    /// Append decompressed bytes to `out` by advancing the C stream one step.
    /// Mirrors the Go reader's feed/drain/finish state machine.
    fn pump(&mut self) -> io::Result<()> {
        // Finish phase: input exhausted, flush trailing output.
        if self.input_done && !self.draining {
            let mut out_len = self.scratch.len();
            // SAFETY: valid handle + scratch; out_len in/out.
            let st = unsafe {
                ffi::cu_decompress_stream_finish(
                    self.stream,
                    self.scratch.as_mut_ptr(),
                    &mut out_len,
                )
            };
            if out_len > 0 {
                self.out.extend_from_slice(&self.scratch[..out_len]);
            }
            if st == ffi::CU_OK {
                self.finished = true;
                self.settle_single();
                return Ok(());
            }
            if st == ffi::CU_ERR_BUF_TOO_SMALL {
                if out_len == 0 {
                    return Err(stuck_error());
                }
                return Ok(()); // more finish output next pump
            }
            return Err(crate::make_error(st).into());
        }

        // Decide what to feed: nothing while draining retained input, else a
        // fresh chunk from src.
        let mut fed = 0usize;
        if !self.draining {
            let src = self.src.as_mut().expect("source present until into_parts");
            let n = src.read(&mut self.in_buf)?;
            if n == 0 {
                self.input_done = true;
                return Ok(()); // re-enter in the finish phase
            }
            fed = n;
        }

        let mut out_len = self.scratch.len();
        let (in_ptr, in_len) = if self.draining {
            (std::ptr::null(), 0usize)
        } else {
            (self.in_buf.as_ptr(), fed)
        };
        // SAFETY: in/out pointers valid for their lengths; out_len in/out.
        let st = unsafe {
            ffi::cu_decompress_stream_write(
                self.stream,
                in_ptr,
                in_len,
                self.scratch.as_mut_ptr(),
                &mut out_len,
            )
        };
        if out_len > 0 {
            self.out.extend_from_slice(&self.scratch[..out_len]);
        }
        match st {
            ffi::CU_OK => {
                self.draining = false;
                self.settle_single();
                Ok(())
            }
            ffi::CU_ERR_BUF_TOO_SMALL => {
                if out_len == 0 {
                    return Err(stuck_error());
                }
                self.draining = true; // unconsumed input remains; drain next
                Ok(())
            }
            other => Err(crate::make_error(other).into()),
        }
    }
}

/// [`Decompressor::into_parts`] before the stream ended and all of its output
/// was read. [`into_inner`](Self::into_inner) returns the decompressor, which
/// keeps reading where it left off.
pub struct IntoPartsError<R: Read>(Decompressor<R>);

impl<R: Read> IntoPartsError<R> {
    /// The decompressor, unchanged.
    pub fn into_inner(self) -> Decompressor<R> {
        self.0
    }
}

impl<R: Read> fmt::Debug for IntoPartsError<R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("IntoPartsError")
    }
}

impl<R: Read> fmt::Display for IntoPartsError<R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("compress-utils: into_parts before the stream ended and its output was read")
    }
}

impl<R: Read> std::error::Error for IntoPartsError<R> {}

impl<R: Read> From<IntoPartsError<R>> for io::Error {
    fn from(e: IntoPartsError<R>) -> io::Error {
        io::Error::new(io::ErrorKind::Other, e.to_string())
    }
}

impl<R: Read> Read for Decompressor<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        while self.out_pos == self.out.len() {
            if let Some(e) = self.pending.take() {
                return Err(e);
            }
            if self.finished {
                return Ok(0);
            }
            // Reset the output accumulator between refills to bound memory.
            self.out.clear();
            self.out_pos = 0;
            if let Err(e) = self.pump() {
                if self.out.is_empty() {
                    return Err(e);
                }
                // Hand back the output decoded before the error first (e.g.
                // the bytes up to a set_max_output limit); the error follows
                // on the next call.
                self.pending = Some(e);
            }
        }
        let n = std::cmp::min(buf.len(), self.out.len() - self.out_pos);
        buf[..n].copy_from_slice(&self.out[self.out_pos..self.out_pos + n]);
        self.out_pos += n;
        Ok(n)
    }
}

impl<R: Read> Drop for Decompressor<R> {
    fn drop(&mut self) {
        if !self.stream.is_null() {
            // SAFETY: handle created by create() and not yet destroyed.
            unsafe { ffi::cu_decompress_stream_destroy(self.stream) };
            self.stream = std::ptr::null_mut();
        }
    }
}
