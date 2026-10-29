//! Self-consistency tests: our output decodes with our input across the
//! one-shot and streaming APIs, for every algorithm compiled into the library.
//! These prove the binding wires the C ABI correctly; interop.rs proves the
//! wire formats match independent implementations.

use std::cell::RefCell;
use std::collections::VecDeque;
use std::io::{self, Read, Write};
use std::rc::Rc;

use compress_utils::{
    compress, decompress, decompress_with_capacity, Algorithm, Compressor, Decompressor, Status,
};

/// Every algorithm the binding exposes. Alias `Lzma` is exercised separately.
const ALGOS: &[Algorithm] = &[
    Algorithm::Zstd,
    Algorithm::Brotli,
    Algorithm::Zlib,
    Algorithm::Bz2,
    Algorithm::Lz4,
    Algorithm::Xz,
    Algorithm::Snappy,
    Algorithm::Gzip,
    Algorithm::Deflate,
];

/// Deterministic pseudo-random bytes (no `rand` dep) — a simple LCG so failures
/// reproduce exactly.
fn pseudo_random(len: usize, seed: u64) -> Vec<u8> {
    let mut s = seed.wrapping_add(0x9E3779B97F4A7C15);
    let mut out = Vec::with_capacity(len);
    for _ in 0..len {
        s = s
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        out.push((s >> 33) as u8);
    }
    out
}

/// A spread of payloads that stress different codec paths: empty (the bz2
/// NULL-source edge), tiny, incompressible, and highly compressible.
fn payloads() -> Vec<(&'static str, Vec<u8>)> {
    vec![
        ("empty", Vec::new()),
        ("one_byte", vec![0x42]),
        (
            "short_text",
            b"the quick brown fox jumps over the lazy dog".to_vec(),
        ),
        ("repetitive", vec![b'A'; 100_000]),
        ("incompressible", pseudo_random(100_000, 1)),
        ("mixed", {
            let mut v = b"header:".to_vec();
            v.extend(vec![b'x'; 50_000]);
            v.extend(pseudo_random(50_000, 2));
            v
        }),
    ]
}

fn available(algos: &[Algorithm]) -> Vec<Algorithm> {
    algos.iter().copied().filter(|a| a.available()).collect()
}

#[test]
fn introspection() {
    // version() is non-empty and dotted.
    let v = compress_utils::version();
    assert!(v.split('.').count() >= 3, "version looks wrong: {v:?}");

    assert_eq!(Algorithm::Zstd.name(), "zstd");
    assert_eq!(Algorithm::Gzip.name(), "gzip");
    // Lzma is an alias for xz.
    assert_eq!(Algorithm::Lzma.name(), "xz");

    // At least one algorithm must be compiled in.
    assert!(!available(ALGOS).is_empty(), "no algorithms available");
}

#[test]
fn one_shot_roundtrip() {
    for algo in available(ALGOS) {
        for (name, data) in payloads() {
            for level in [1, 5, 10] {
                let packed = compress(algo, &data, level)
                    .unwrap_or_else(|e| panic!("{algo} compress {name} L{level}: {e}"));
                let restored = decompress(algo, &packed)
                    .unwrap_or_else(|e| panic!("{algo} decompress {name} L{level}: {e}"));
                assert_eq!(
                    restored, data,
                    "{algo} roundtrip mismatch on {name} at level {level}"
                );
            }
        }
    }
}

#[test]
fn compress_bound_is_upper_bound() {
    for algo in available(ALGOS) {
        for (_, data) in payloads() {
            let packed = compress(algo, &data, 5).unwrap();
            assert!(
                packed.len() <= algo.compress_bound(data.len()),
                "{algo}: compressed {} exceeds bound {}",
                packed.len(),
                algo.compress_bound(data.len())
            );
        }
    }
}

/// Streaming compress → one-shot decompress, and one-shot compress → streaming
/// decompress. This is the check that catches wire-format divergence between
/// the two code paths (the class of bug that hid the legacy LZ4 issue).
#[test]
fn cross_api() {
    for algo in available(ALGOS) {
        for (name, data) in payloads() {
            // Stream-compress in several small writes, then one-shot decompress.
            let mut c = Compressor::new(Vec::new(), algo, 6).unwrap();
            for chunk in data.chunks(7).chain(std::iter::once(&data[0..0])) {
                c.write_all(chunk).unwrap();
            }
            let streamed = c.finish().unwrap();
            let restored = decompress(algo, &streamed)
                .unwrap_or_else(|e| panic!("{algo} stream->oneshot {name}: {e}"));
            assert_eq!(restored, data, "{algo} stream->oneshot {name}");

            // One-shot compress, then stream-decompress in small reads.
            let packed = compress(algo, &data, 6).unwrap();
            let mut d = Decompressor::new(&packed[..], algo).unwrap();
            let mut restored2 = Vec::new();
            d.read_to_end(&mut restored2)
                .unwrap_or_else(|e| panic!("{algo} oneshot->stream {name}: {e}"));
            assert_eq!(restored2, data, "{algo} oneshot->stream {name}");
        }
    }
}

/// A larger streaming roundtrip that forces many internal drain cycles.
#[test]
fn streaming_large() {
    let data = {
        let mut v = pseudo_random(2_000_000, 7);
        // Interleave a compressible run so both codec branches exercise.
        v.extend(vec![b'Z'; 500_000]);
        v
    };
    for algo in available(ALGOS) {
        let mut c = Compressor::new(Vec::new(), algo, 4).unwrap();
        c.write_all(&data).unwrap();
        let packed = c.finish().unwrap();

        let mut d = Decompressor::new(&packed[..], algo).unwrap();
        let mut restored = Vec::new();
        d.read_to_end(&mut restored).unwrap();
        assert_eq!(restored.len(), data.len(), "{algo} large size");
        assert!(restored == data, "{algo} large content mismatch");
    }
}

/// Truncated / malformed input must error, never panic or silently succeed.
#[test]
fn rejects_bad_input() {
    for algo in available(ALGOS) {
        // A 1-byte buffer cannot legitimately encode non-empty data. Most
        // codecs reject it; Snappy's `0x00` is a valid empty block, so allow an
        // empty Ok but never a non-empty decode.
        if let Ok(out) = decompress(algo, &[0x00]) {
            assert!(
                out.is_empty(),
                "{algo} decoded a 1-byte buffer to {} bytes",
                out.len()
            );
        }

        // A valid buffer truncated to its first byte.
        let packed = compress(algo, b"some real content here", 5).unwrap();
        if packed.len() > 4 {
            let truncated = &packed[..packed.len() / 4];
            // Must not panic; snappy raw block may decode a prefix, so only
            // assert it doesn't return the full original.
            let r = decompress(algo, truncated);
            if let Ok(out) = r {
                assert_ne!(
                    out, b"some real content here",
                    "{algo} decoded truncated input as original"
                );
            }
        }
    }
}

/// The alias Lzma must produce output the Xz path decodes (same codec).
#[test]
fn lzma_alias() {
    if !Algorithm::Xz.available() {
        return;
    }
    let data = b"lzma alias should equal xz".to_vec();
    let packed = compress(Algorithm::Lzma, &data, 5).unwrap();
    assert_eq!(decompress(Algorithm::Xz, &packed).unwrap(), data);
}

/// A capacity is an upper bound: exact and oversized capacities decode the
/// same bytes, and an undersized one errors.
#[test]
fn decompress_with_capacity_bounds() {
    let data = pseudo_random(16_000, 3);
    for algo in available(ALGOS) {
        let packed = compress(algo, &data, 5).unwrap();
        for capacity in [data.len(), data.len() + 4096] {
            let out = decompress_with_capacity(algo, &packed, capacity)
                .unwrap_or_else(|e| panic!("{algo} capacity {capacity}: {e}"));
            assert_eq!(out, data, "{algo} capacity {capacity}");
        }
        assert!(
            decompress_with_capacity(algo, &packed, data.len() / 2).is_err(),
            "{algo} accepted an undersized capacity"
        );
    }
}

#[test]
fn lz4_raw_supports_capacity_and_stream_fallback() {
    if !Algorithm::Lz4Raw.available() {
        return;
    }
    for (name, data) in payloads() {
        let block = compress(Algorithm::Lz4Raw, &data, 5).unwrap();
        let out = decompress_with_capacity(Algorithm::Lz4Raw, &block, data.len() + 100)
            .unwrap_or_else(|e| panic!("lz4_raw {name}: {e}"));
        assert_eq!(out, data, "lz4_raw {name}");
    }
    let block = compress(Algorithm::Lz4Raw, b"hello hello hello", 5).unwrap();
    assert_eq!(
        decompress(Algorithm::Lz4Raw, &block).unwrap(),
        b"hello hello hello"
    );
    let err = decompress_with_capacity(Algorithm::Lz4Raw, &block, 4).unwrap_err();
    assert_eq!(err.code, Status::BufTooSmall, "{err}");
}

#[test]
fn lzo_decodes_only() {
    if !Algorithm::Lzo.available() {
        return;
    }
    // Independent LZO1X block: five literals ("hello") then the end marker.
    let block = [22, b'h', b'e', b'l', b'l', b'o', 17, 0, 0];
    assert_eq!(decompress(Algorithm::Lzo, &block).unwrap(), b"hello");
    assert_eq!(
        decompress_with_capacity(Algorithm::Lzo, &block, 64).unwrap(),
        b"hello"
    );
    let err = compress(Algorithm::Lzo, b"hello", 5).unwrap_err();
    assert_eq!(err.code, Status::UnsupportedAlgo, "{err}");
}

/// One end of an in-memory pipe that behaves like a non-blocking socket: the
/// reader gets the bytes written so far, at most `limit` per read when it's
/// nonzero (like a fragmenting transport), `WouldBlock` when there are none
/// yet, and end of stream once the writer is closed.
#[derive(Clone, Default)]
struct Pipe {
    state: Rc<RefCell<(VecDeque<u8>, bool)>>,
    limit: usize,
}

impl Pipe {
    fn with_limit(limit: usize) -> Self {
        Pipe {
            limit,
            ..Pipe::default()
        }
    }
    fn pending(&self) -> usize {
        self.state.borrow().0.len()
    }
    fn close(&self) {
        self.state.borrow_mut().1 = true;
    }
}

impl Write for Pipe {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.state.borrow_mut().0.extend(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Read for Pipe {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let mut state = self.state.borrow_mut();
        if state.0.is_empty() {
            return if state.1 {
                Ok(0)
            } else {
                Err(io::ErrorKind::WouldBlock.into())
            };
        }
        let mut n = buf.len().min(state.0.len());
        if self.limit > 0 {
            n = n.min(self.limit);
        }
        for (dst, src) in buf.iter_mut().zip(state.0.drain(..n)) {
            *dst = src;
        }
        Ok(n)
    }
}

#[test]
fn flush_delivers_each_message() {
    // The random message doesn't compress, so flushing it takes more than one
    // 64 KiB output buffer: flush()'s drain loop must run again.
    let messages = [
        vec![b'A'; 131_072],
        pseudo_random(65_537, 3),
        b"B".repeat(37),
    ];
    for algo in available(ALGOS) {
        if matches!(algo, Algorithm::Snappy | Algorithm::Bz2) {
            let mut tx = Compressor::new(Pipe::default(), algo, 5).unwrap();
            tx.write_all(b"abc").unwrap();
            let err = tx.flush().expect_err("flush must fail");
            assert_eq!(err.kind(), io::ErrorKind::Unsupported, "{algo:?}");
            continue;
        }
        for limit in [0, 1, 4093] {
            let pipe = Pipe::with_limit(limit);
            let mut tx = Compressor::new(pipe.clone(), algo, 5).unwrap();
            let mut rx = Decompressor::new(pipe.clone(), algo).unwrap();
            for (i, message) in messages.iter().enumerate() {
                tx.write_all(message).unwrap();
                let before = pipe.pending();
                tx.flush().unwrap();
                if algo == Algorithm::Zstd && message.len() == 65_537 {
                    let flushed = pipe.pending() - before;
                    assert!(flushed > 64 * 1024, "zstd flush wrote only {flushed} bytes");
                }
                // Only the flushed bytes are available: reading would block if
                // the decoder needed anything more to produce this message.
                let mut got = vec![0u8; message.len()];
                rx.read_exact(&mut got)
                    .unwrap_or_else(|e| panic!("{algo:?} read limit {limit}, message {i}: {e}"));
                assert_eq!(&got, message, "{algo:?} read limit {limit}, message {i}");
            }
            tx.finish().unwrap();
            pipe.close();
            let mut rest = Vec::new();
            rx.read_to_end(&mut rest).unwrap();
            assert!(
                rest.is_empty(),
                "{algo:?} read limit {limit}: output after finish"
            );
        }
    }
}

/// A source that hands out its bytes in pieces of the given sizes, cycled (the
/// last piece of each read may be shorter): lets a test choose exactly where
/// the decompressor's reads from `src` fall relative to the end of a frame.
struct Pieces {
    data: Vec<u8>,
    pos: usize,
    sizes: Vec<usize>,
    next: usize,
}

impl Pieces {
    fn new(data: Vec<u8>, sizes: &[usize]) -> Self {
        Pieces {
            data,
            pos: 0,
            sizes: sizes.to_vec(),
            next: 0,
        }
    }
}

impl Read for Pieces {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let size = self.sizes[self.next % self.sizes.len()];
        self.next += 1;
        let n = buf.len().min(size).min(self.data.len() - self.pos);
        buf[..n].copy_from_slice(&self.data[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
}

const TRAILER: &[u8] = b"ZPKL\x01\x01\x03\x00 next message";

/// Decodes one frame in single-stream mode, then returns the decoded bytes and
/// everything after the frame, read as `unused` followed by the source.
fn single_then_rest<R: Read>(src: R, algo: Algorithm) -> (Vec<u8>, Vec<u8>) {
    let mut d = Decompressor::single_stream(src, algo).unwrap();
    assert!(!d.is_eof() && d.unused().is_empty());
    let mut got = Vec::new();
    d.read_to_end(&mut got)
        .unwrap_or_else(|e| panic!("{algo:?}: {e}"));
    assert!(d.is_eof(), "{algo:?}: no eof after read_to_end");
    let mut buf = [0u8; 16];
    assert_eq!(d.read(&mut buf).unwrap(), 0, "{algo:?}: read after eof");
    let unused_copy = d.unused().to_vec();
    let (src, unused) = d.into_parts().unwrap();
    assert_eq!(unused, unused_copy);
    let mut rest = Vec::new();
    io::Cursor::new(unused)
        .chain(src)
        .read_to_end(&mut rest)
        .unwrap();
    (got, rest)
}

#[test]
fn single_stream_stops_at_end_of_frame() {
    for algo in available(ALGOS) {
        for (name, payload) in [
            ("small", b"single stream ".repeat(70)),
            ("200k random", pseudo_random(200_000, 7)),
        ] {
            let frame = compress(algo, &payload, 5).unwrap();
            let mut wire = frame.clone();
            wire.extend_from_slice(TRAILER);
            let n = frame.len();
            // The frame ends partway through a read (whole source, and splits one
            // byte either side of the end), exactly at a read boundary, and, for
            // the small payload, with the source handing out one byte at a time.
            let mut splits = vec![
                vec![usize::MAX],
                vec![n],
                vec![n - 1, usize::MAX],
                vec![n + 1],
            ];
            if payload.len() < 2000 {
                splits.push(vec![1]);
            }
            for sizes in splits {
                let (got, rest) = single_then_rest(Pieces::new(wire.clone(), &sizes), algo);
                assert!(
                    got == payload,
                    "{algo:?} {name} reads {sizes:?}: wrong output"
                );
                assert_eq!(rest, TRAILER, "{algo:?} {name} reads {sizes:?}: wrong rest");
            }
        }
    }
}

#[test]
fn single_stream_reports_unused_read_ahead() {
    // A small frame inside one 64 KiB read: the rest of that read is unused.
    for algo in available(ALGOS) {
        let mut wire = compress(algo, b"hello", 5).unwrap();
        let n = wire.len();
        wire.extend_from_slice(TRAILER);
        let mut d = Decompressor::single_stream(io::Cursor::new(wire), algo).unwrap();
        let mut got = Vec::new();
        d.read_to_end(&mut got).unwrap();
        assert_eq!(got, b"hello");
        assert!(d.is_eof());
        assert_eq!(d.unused(), TRAILER, "{algo:?}");
        let (src, _) = d.into_parts().unwrap();
        assert_eq!(src.position() as usize, n + TRAILER.len());
    }
    // The default mode never reports eof or unused bytes.
    let wire = compress(Algorithm::Zstd, b"hello", 5).unwrap();
    let mut d = Decompressor::new(wire.as_slice(), Algorithm::Zstd).unwrap();
    let mut got = Vec::new();
    d.read_to_end(&mut got).unwrap();
    assert!(!d.is_eof() && d.unused().is_empty());
}

#[test]
fn single_stream_back_to_back_frames() {
    for algo in available(ALGOS) {
        let first = pseudo_random(3000, 11);
        let second = b"second frame ".repeat(300);
        let mut wire = compress(algo, &first, 5).unwrap();
        wire.extend(compress(algo, &second, 5).unwrap());
        wire.extend_from_slice(TRAILER);
        let (got, rest) = single_then_rest(wire.as_slice(), algo);
        assert!(got == first, "{algo:?}: first frame");
        let (got2, rest2) = single_then_rest(rest.as_slice(), algo);
        assert!(
            got2 == second,
            "{algo:?}: second frame from unused + source"
        );
        assert_eq!(rest2, TRAILER, "{algo:?}");
    }
}

#[test]
fn single_stream_large_read_ahead() {
    // The source is far bigger than one internal read, so the decompressor has
    // read past the end of the frame while most of the source is still unread:
    // unused plus the source must still be exactly what followed the frame.
    let big_tail = pseudo_random(3 * 1024 * 1024, 5);
    let big_first = b"large first frame, ".repeat(60_000);
    let big_second = pseudo_random(2 * 1024 * 1024, 9);
    for algo in available(ALGOS) {
        let mut wire = compress(algo, b"small", 5).unwrap();
        wire.extend_from_slice(&big_tail);
        let (got, rest) = single_then_rest(io::Cursor::new(wire), algo);
        assert_eq!(got, b"small", "{algo:?}");
        assert!(rest == big_tail, "{algo:?}: lost bytes after a small frame");

        // Two big frames, read through an irregular transport.
        let mut wire = compress(algo, &big_first, 5).unwrap();
        wire.extend(compress(algo, &big_second, 5).unwrap());
        let (got, rest) = single_then_rest(Pieces::new(wire, &[4093, 97, 65_536, 7]), algo);
        assert!(got == big_first, "{algo:?}: big first frame");
        let (got2, rest2) = single_then_rest(rest.as_slice(), algo);
        assert!(got2 == big_second, "{algo:?}: big second frame");
        assert!(rest2.is_empty(), "{algo:?}");
    }
}

#[test]
fn single_stream_truncated_and_unsupported() {
    for algo in available(ALGOS) {
        let frame = compress(algo, &pseudo_random(1000, 3), 5).unwrap();
        let mut d = Decompressor::single_stream(&frame[..frame.len() - 1], algo).unwrap();
        let mut got = Vec::new();
        assert!(
            d.read_to_end(&mut got).is_err(),
            "{algo:?}: truncated frame"
        );
        assert!(!d.is_eof() && d.unused().is_empty(), "{algo:?}");
    }
    for algo in available(&[Algorithm::Lz4Raw, Algorithm::Lzo]) {
        match Decompressor::single_stream(&b""[..], algo) {
            Err(e) => assert_eq!(e.code, Status::UnsupportedAlgo, "{algo:?}: {e}"),
            Ok(_) => panic!("{algo:?}: single-stream mode must be unsupported"),
        }
    }
}

#[test]
fn single_stream_into_parts_before_the_end_hands_the_decompressor_back() {
    // A caller that stops reading at the end of its own data (read_exact, as
    // pickle stops at its STOP opcode) mustn't lose the rest of the frame or
    // the bytes after it: into_parts refuses until read has returned Ok(0).
    let message = pseudo_random(100_000, 77);
    for algo in available(ALGOS) {
        let mut wire = compress(algo, &message, 5).unwrap();
        wire.extend_from_slice(TRAILER);
        let mut d = Decompressor::single_stream(io::Cursor::new(wire), algo).unwrap();

        let mut got = vec![0u8; message.len()];
        d.read_exact(&mut got[..message.len() / 2]).unwrap();
        let err = match d.into_parts() {
            Ok(_) => panic!("{algo:?}: into_parts with half the output unread"),
            Err(e) => e,
        };
        assert!(err.to_string().contains("into_parts"));
        let mut d = err.into_inner();
        d.read_exact(&mut got[message.len() / 2..]).unwrap();
        assert_eq!(got, message, "{algo:?}");

        // All of the message has been read, but maybe not Ok(0) yet.
        let (src, unused) = match d.into_parts() {
            Ok(parts) => parts,
            Err(e) => {
                let mut d = e.into_inner();
                let mut extra = Vec::new();
                d.read_to_end(&mut extra).unwrap();
                assert!(extra.is_empty(), "{algo:?}: output past the message");
                d.into_parts().unwrap()
            }
        };
        let mut rest = Vec::new();
        io::Cursor::new(unused)
            .chain(src)
            .read_to_end(&mut rest)
            .unwrap();
        assert_eq!(rest, TRAILER, "{algo:?}");
    }
}

#[test]
fn algorithm_display_is_its_name() {
    let all = ALGOS
        .iter()
        .copied()
        .chain([Algorithm::Lzma, Algorithm::Lz4Raw, Algorithm::Lzo]);
    for algo in all {
        assert!(!algo.name().is_empty(), "{algo:?} has no name");
        assert_eq!(algo.to_string(), algo.name(), "{algo:?}");
    }
    assert_eq!(format!("{}", Algorithm::Lzma), "xz");
}

/// Levels outside 1..=10 are rejected by both the one-shot and streaming APIs,
/// with the C status and its message carried on the error.
#[test]
fn rejects_out_of_range_levels() {
    for algo in available(ALGOS) {
        for level in [-1, 0, 11] {
            let err = compress(algo, b"data", level).unwrap_err();
            assert_eq!(err.code, Status::InvalidLevel, "{algo} L{level}: {err}");
            assert_eq!(err.to_string(), err.message);
            assert!(
                err.message
                    .starts_with("compression level out of range (1..10)"),
                "{algo} L{level}: {err}"
            );
            match Compressor::new(Vec::new(), algo, level) {
                Err(e) => assert_eq!(e.code, Status::InvalidLevel, "{algo} L{level}: {e}"),
                Ok(_) => panic!("{algo}: stream accepted level {level}"),
            }
        }
    }
}

/// An error message is the status text, plus `": "` and the C detail only
/// when the C core recorded one.
#[test]
fn error_message_joins_status_and_detail() {
    // LZ4 and Snappy reject empty input in the size probe without a detail.
    for algo in available(&[Algorithm::Lz4, Algorithm::Snappy]) {
        let err = decompress(algo, b"").unwrap_err();
        assert_eq!(err.code, Status::Truncated, "{algo}: {err}");
        assert_eq!(err.message, "compressed input truncated", "{algo}");
    }
    if Algorithm::Zstd.available() {
        let err = decompress(Algorithm::Zstd, b"").unwrap_err();
        assert_eq!(err.code, Status::Truncated, "{err}");
        assert_eq!(err.message, "compressed input truncated: zstd: empty input");
    }
}

/// A capacity that can't be allocated is reported as `Oom` before calling
/// into C, not as a panic or an abort.
#[test]
fn unallocatable_capacity_is_oom() {
    for algo in available(ALGOS) {
        let packed = compress(algo, b"capacity", 5).unwrap();
        let err = decompress_with_capacity(algo, &packed, usize::MAX).unwrap_err();
        assert_eq!(err.code, Status::Oom, "{algo}: {err}");
        assert!(
            err.message.contains(&usize::MAX.to_string()),
            "{algo}: {err}"
        );
    }
}

/// A zstd header declaring an absurd content size makes the one-shot path
/// unable to reserve the output; `decompress` must fall back and reject the
/// input rather than abort on allocation.
#[test]
fn absurd_declared_size_is_rejected_not_allocated() {
    if !Algorithm::Zstd.available() {
        return;
    }
    // Magic, then a frame header descriptor with an 8-byte content size and
    // the single-segment flag (no window descriptor), then the size: 2^63.
    let mut frame = vec![0x28, 0xB5, 0x2F, 0xFD, 0xE0];
    frame.extend_from_slice(&(1u64 << 63).to_le_bytes());
    let err = decompress(Algorithm::Zstd, &frame).unwrap_err();
    assert!(
        matches!(
            err.code,
            Status::Decompression | Status::Truncated | Status::SizeLimit
        ),
        "{err:?}"
    );
}

/// A complete LZ4 frame followed by a truncated one: the size probe only
/// knows the first frame's size, so the one-shot decode runs out of room and
/// `decompress` falls back to streaming, which must report the truncation
/// rather than return the first frame alone.
#[test]
fn lz4_frame_then_truncated_frame_is_truncated() {
    if !Algorithm::Lz4.available() {
        return;
    }
    let first = b"a complete first frame".to_vec();
    // Several 64 KiB blocks, so the second frame has output to deliver.
    let second = pseudo_random(300_000, 21);
    let mut wire = compress(Algorithm::Lz4, &first, 5).unwrap();
    let second_frame = compress(Algorithm::Lz4, &second, 5).unwrap();

    let mut whole = wire.clone();
    whole.extend_from_slice(&second_frame);
    let mut expected = first.clone();
    expected.extend_from_slice(&second);
    assert!(decompress(Algorithm::Lz4, &whole).unwrap() == expected);

    wire.extend_from_slice(&second_frame[..second_frame.len() - 1]);
    let err = decompress(Algorithm::Lz4, &wire).unwrap_err();
    assert_eq!(err.code, Status::Truncated, "{err}");
}

/// `write(&[])` writes nothing and leaves the stream usable.
#[test]
fn empty_write_is_a_no_op() {
    let data = b"payload after empty writes".to_vec();
    for algo in available(ALGOS) {
        let mut c = Compressor::new(Vec::new(), algo, 5).unwrap();
        assert_eq!(c.write(&[]).unwrap(), 0, "{algo}");
        c.write_all(&data).unwrap();
        assert_eq!(c.write(&[]).unwrap(), 0, "{algo}");
        let packed = c.finish().unwrap();
        assert_eq!(decompress(algo, &packed).unwrap(), data, "{algo}");
    }
}

/// A writer that always fails, like a closed socket.
struct BrokenSink;

impl Write for BrokenSink {
    fn write(&mut self, _: &[u8]) -> io::Result<usize> {
        Err(io::ErrorKind::BrokenPipe.into())
    }
    fn flush(&mut self) -> io::Result<()> {
        Err(io::ErrorKind::BrokenPipe.into())
    }
}

/// The sink's I/O errors reach the caller unchanged from `write` and from
/// `finish` (which always has output to deliver), and the drop that follows
/// a failed `finish` doesn't panic.
#[test]
fn compressor_propagates_sink_errors() {
    let data = pseudo_random(1_000_000, 5);
    for algo in available(ALGOS) {
        let mut c = Compressor::new(BrokenSink, algo, 1).unwrap();
        // Codecs that buffer the input emit nothing yet, so write may succeed.
        if let Err(e) = c.write_all(&data) {
            assert_eq!(e.kind(), io::ErrorKind::BrokenPipe, "{algo}: {e}");
        }
        let err = c.finish().map(|_| ()).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::BrokenPipe, "{algo}: {err}");

        let c = Compressor::new(BrokenSink, algo, 5).unwrap();
        let err = c.finish().map(|_| ()).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::BrokenPipe, "{algo}: {err}");
    }
}

/// A reader that always fails.
struct BrokenSource;

impl Read for BrokenSource {
    fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
        Err(io::ErrorKind::ConnectionReset.into())
    }
}

#[test]
fn decompressor_propagates_source_errors() {
    for algo in available(ALGOS) {
        let mut d = Decompressor::new(BrokenSource, algo).unwrap();
        let err = d.read(&mut [0u8; 16]).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::ConnectionReset, "{algo}: {err}");
    }
}

/// `IntoPartsError` formats for `{:?}` and `{}` and converts into an
/// `io::Error`, so `?` works in functions returning `io::Result`.
#[test]
fn into_parts_error_formats_and_converts() {
    fn take_parts(d: Decompressor<&[u8]>) -> io::Result<Vec<u8>> {
        let (_, unused) = d.into_parts()?;
        Ok(unused)
    }
    for algo in available(ALGOS) {
        let packed = compress(algo, b"into parts", 5).unwrap();
        let err = Decompressor::new(&packed[..], algo)
            .unwrap()
            .into_parts()
            .unwrap_err();
        assert_eq!(format!("{err:?}"), "IntoPartsError");
        let display = err.to_string();
        assert!(display.contains("into_parts"), "{display}");

        let io_err: io::Error = err.into();
        assert_eq!(io_err.kind(), io::ErrorKind::Other);
        assert_eq!(io_err.to_string(), display);

        let err = take_parts(Decompressor::new(&packed[..], algo).unwrap()).unwrap_err();
        assert_eq!(err.to_string(), display, "{algo}");

        // Read to the end, and the parts are available (with nothing unused,
        // as always for a decompressor made with `new`).
        let mut d = Decompressor::new(&packed[..], algo).unwrap();
        let mut out = Vec::new();
        d.read_to_end(&mut out).unwrap();
        assert_eq!(out, b"into parts", "{algo}");
        assert!(take_parts(d).unwrap().is_empty(), "{algo}");
    }
}

#[test]
fn decompressor_max_output() {
    let data = pseudo_random(8192, 21);
    for &algo in &available(ALGOS) {
        let packed = compress(algo, &data, 5).unwrap();

        // One byte under: the bytes up to the limit, then SizeLimit.
        let mut d = Decompressor::new(&packed[..], algo).unwrap();
        d.set_max_output(data.len() - 1);
        let mut out = Vec::new();
        let err = d.read_to_end(&mut out).unwrap_err();
        let inner = err
            .get_ref()
            .and_then(|e| e.downcast_ref::<compress_utils::Error>())
            .unwrap_or_else(|| panic!("{algo}: not a library error: {err}"));
        assert_eq!(inner.code, Status::SizeLimit, "{algo}: {inner}");
        assert_eq!(out, data[..data.len() - 1], "{algo}");

        // Exactly the limit decodes everything.
        let mut d = Decompressor::new(&packed[..], algo).unwrap();
        d.set_max_output(data.len());
        let mut out = Vec::new();
        d.read_to_end(&mut out).unwrap();
        assert_eq!(out, data, "{algo}");
    }
}

/// Set in the environment of the child process that runs
/// `size_cap_in_child_process`.
const SIZE_CAP_CHILD: &str = "COMPRESS_UTILS_SIZE_CAP_CHILD";

/// `set_max_decompressed_size` is process-global and the tests in this binary
/// run in parallel, so the test that changes it runs alone in a child process:
/// this binary again, filtered to that one test.
#[test]
fn decompressed_size_cap() {
    let out = std::process::Command::new(std::env::current_exe().unwrap())
        .args(["--exact", "size_cap_in_child_process", "--ignored"])
        .args(["--test-threads", "1", "--nocapture"])
        .env(SIZE_CAP_CHILD, "1")
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&out.stdout);
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(
        out.status.success() && stdout.contains("1 passed"),
        "child failed or didn't run the test:\n{stdout}\n{stderr}"
    );
}

/// The C core's default cap (CU_DEFAULT_MAX_DECOMPRESSED_SIZE), restored on
/// drop so the cap is put back even if an assertion fails.
struct RestoreDefaultCap;

impl Drop for RestoreDefaultCap {
    fn drop(&mut self) {
        compress_utils::set_max_decompressed_size(1024 * 1024 * 1024);
    }
}

#[test]
#[ignore = "changes a process-global setting; run in its own process by decompressed_size_cap"]
fn size_cap_in_child_process() {
    if std::env::var_os(SIZE_CAP_CHILD).is_none() {
        return; // run directly (e.g. --include-ignored) alongside other tests
    }
    let _restore = RestoreDefaultCap;
    let cap = 4096;
    let at_cap = pseudo_random(cap, 11);
    let over_cap = pseudo_random(cap + 1, 12);
    let packed = |algo, data: &[u8]| compress(algo, data, 5).unwrap();
    let mut algos = available(ALGOS);
    algos.extend(available(&[Algorithm::Lz4Raw]));

    compress_utils::set_max_decompressed_size(cap);
    for &algo in &algos {
        // At the cap is fine, one byte over is rejected, whether the C
        // one-shot path (size-recording formats) or the binding's streaming
        // fallback (zlib, gzip, brotli, ...) enforces it.
        assert_eq!(
            decompress(algo, &packed(algo, &at_cap)).unwrap(),
            at_cap,
            "{algo}"
        );
        let err = decompress(algo, &packed(algo, &over_cap)).unwrap_err();
        assert_eq!(err.code, Status::SizeLimit, "{algo}: {err}");
        assert!(err.message.contains("exceeds"), "{algo}: {err}");
    }
    // The stream behind the fallback names the cap it enforced.
    if Algorithm::Gzip.available() {
        let err = decompress(Algorithm::Gzip, &packed(Algorithm::Gzip, &over_cap)).unwrap_err();
        assert!(err.message.contains(&cap.to_string()), "{err}");

        // The streaming Decompressor is not bound by the one-shot cap.
        let gz = packed(Algorithm::Gzip, &over_cap);
        let mut d = Decompressor::new(&gz[..], Algorithm::Gzip).unwrap();
        let mut out = Vec::new();
        d.read_to_end(&mut out).unwrap();
        assert_eq!(out, over_cap);
    }
    // Single-stream Snappy rejects a block declaring more than the cap.
    if Algorithm::Snappy.available() {
        let sn = packed(Algorithm::Snappy, &over_cap);
        let mut d = Decompressor::single_stream(&sn[..], Algorithm::Snappy).unwrap();
        let err = d.read_to_end(&mut Vec::new()).unwrap_err();
        let inner = err
            .get_ref()
            .and_then(|e| e.downcast_ref::<compress_utils::Error>())
            .unwrap_or_else(|| panic!("not a library error: {err}"));
        assert_eq!(inner.code, Status::SizeLimit, "{inner}");
    }

    // 0 disables the cap everywhere.
    compress_utils::set_max_decompressed_size(0);
    for &algo in &algos {
        assert_eq!(
            decompress(algo, &packed(algo, &over_cap)).unwrap(),
            over_cap,
            "{algo}"
        );
    }
}

/// One-shot xz caps the dictionary at the input size (4 KiB floor, never above
/// the preset's): round trips at, one byte under and one byte over each cap, with
/// text and incompressible data, on levels with different preset dictionaries.
#[test]
fn xz_fitted_dictionary_round_trips() {
    if !Algorithm::Xz.available() {
        return;
    }
    let text = |n: usize| -> Vec<u8> {
        b"The quick brown fox jumps over the lazy dog. "
            .iter()
            .copied()
            .cycle()
            .take(n)
            .collect()
    };
    for level in [1, 6, 10] {
        for n in [
            0usize, 1, 4095, 4096, 4097, 8191, 8192, 8193, 65535, 65536, 65537,
        ] {
            for (kind, data) in [("text", text(n)), ("noise", pseudo_random(n, n as u64))] {
                let packed = compress(Algorithm::Xz, &data, level).unwrap();
                let back = decompress(Algorithm::Xz, &packed).unwrap();
                assert_eq!(back, data, "xz level {level} {kind} n={n}");
            }
        }
    }
    // Larger than level 1's 256 KiB dictionary: the cap must not apply.
    let big = pseudo_random(300_000, 7);
    let packed = compress(Algorithm::Xz, &big, 1).unwrap();
    assert_eq!(decompress(Algorithm::Xz, &packed).unwrap(), big);
}
