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

/// Recover a library [`Error`] from an [`io::Error`] produced by the streaming
/// adapters (used by the top-level [`crate::decompress`] fallback).
pub(crate) fn io_to_error(e: io::Error) -> Error {
    let fallback = e.to_string();
    match e.into_inner() {
        Some(b) => match b.downcast::<Error>() {
            Ok(err) => *err,
            Err(b) => Error {
                code: Status::Internal,
                message: b.to_string(),
            },
        },
        None => Error {
            code: Status::Internal,
            message: fallback,
        },
    }
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

    fn flush(&mut self) -> io::Result<()> {
        // Data is forwarded to the sink on every write; the C compress stream
        // has no mid-stream flush that keeps the frame open, so we only flush
        // the sink. Call finish() to finalize the frame.
        if let Some(sink) = self.sink.as_mut() {
            sink.flush()
        } else {
            Ok(())
        }
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
pub struct Decompressor<R: Read> {
    src: R,
    stream: *mut ffi::cu_decompress_stream,
    in_buf: Vec<u8>,
    scratch: Vec<u8>,
    out: Vec<u8>,
    out_pos: usize,

    input_done: bool,
    draining: bool,
    finished: bool,
}

impl<R: Read> Decompressor<R> {
    /// Create a decompressor reading a stream produced by `algo` from `src`.
    pub fn new(src: R, algo: Algorithm) -> Result<Self, Error> {
        let mut stream: *mut ffi::cu_decompress_stream = std::ptr::null_mut();
        // SAFETY: out_stream points at a local; set to a valid handle on CU_OK.
        let st = unsafe { ffi::cu_decompress_stream_create(algo.to_raw(), &mut stream) };
        crate::check_status(st)?;
        Ok(Decompressor {
            src,
            stream,
            in_buf: vec![0u8; SCRATCH],
            scratch: vec![0u8; SCRATCH],
            out: Vec::new(),
            out_pos: 0,
            input_done: false,
            draining: false,
            finished: false,
        })
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
            let n = self.src.read(&mut self.in_buf)?;
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

impl<R: Read> Read for Decompressor<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        while self.out_pos == self.out.len() {
            if self.finished {
                return Ok(0);
            }
            // Reset the output accumulator between refills to bound memory.
            self.out.clear();
            self.out_pos = 0;
            self.pump()?;
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
