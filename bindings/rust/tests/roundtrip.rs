//! Self-consistency tests: our output decodes with our input across the
//! one-shot and streaming APIs, for every algorithm compiled into the library.
//! These prove the binding wires the C ABI correctly; interop.rs proves the
//! wire formats match independent implementations.

use std::io::{Read, Write};

use compress_utils::{compress, decompress, Algorithm, Compressor, Decompressor};

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
