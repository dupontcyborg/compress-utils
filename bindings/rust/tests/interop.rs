//! Cross-implementation interop: prove our wire formats match *independent*
//! codecs, not just our own decoder. All references are pure-Rust crates so the
//! test binary never links a second copy of the C codec symbols our archive
//! already contains.
//!
//! Two directions per algorithm where the reference supports both:
//!   * outbound — our compress()  → reference decodes  == input
//!   * inbound  — reference encodes → our decompress()  == input
//!
//! bz2 is absent: no mainstream pure-Rust reference exists. It stays covered by
//! the Python (`bindings/python/tests/test_interop.py`) and CLI channels.

use std::io::{Read, Write};

use compress_utils::{compress, decompress, Algorithm};

fn samples() -> Vec<Vec<u8>> {
    vec![
        b"interop: the quick brown fox jumps over the lazy dog".to_vec(),
        vec![b'A'; 40_000],
        (0..30_000u32)
            .map(|i| (i.wrapping_mul(2654435761) >> 24) as u8)
            .collect(),
    ]
}

// ---- zstd (outbound only; ruzstd is decoder-only) -------------------------

#[test]
fn zstd_outbound_ruzstd() {
    if !Algorithm::Zstd.available() {
        return;
    }
    for data in samples() {
        let packed = compress(Algorithm::Zstd, &data, 6).unwrap();
        let mut dec = ruzstd::StreamingDecoder::new(&packed[..]).expect("ruzstd init");
        let mut out = Vec::new();
        dec.read_to_end(&mut out).expect("ruzstd decode our zstd");
        assert_eq!(
            out, data,
            "zstd outbound: ruzstd could not decode our frame"
        );
    }
}

// ---- gzip (flate2 / miniz_oxide) ------------------------------------------

#[test]
fn gzip_both_ways_flate2() {
    if !Algorithm::Gzip.available() {
        return;
    }
    for data in samples() {
        // outbound
        let packed = compress(Algorithm::Gzip, &data, 6).unwrap();
        let mut d = flate2::read::GzDecoder::new(&packed[..]);
        let mut out = Vec::new();
        d.read_to_end(&mut out).expect("flate2 decode our gzip");
        assert_eq!(out, data, "gzip outbound");

        // inbound
        let mut e = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::new(6));
        e.write_all(&data).unwrap();
        let ref_packed = e.finish().unwrap();
        assert_eq!(
            decompress(Algorithm::Gzip, &ref_packed).unwrap(),
            data,
            "gzip inbound"
        );
    }
}

// ---- zlib (flate2 / miniz_oxide) ------------------------------------------

#[test]
fn zlib_both_ways_flate2() {
    if !Algorithm::Zlib.available() {
        return;
    }
    for data in samples() {
        let packed = compress(Algorithm::Zlib, &data, 6).unwrap();
        let mut d = flate2::read::ZlibDecoder::new(&packed[..]);
        let mut out = Vec::new();
        d.read_to_end(&mut out).expect("flate2 decode our zlib");
        assert_eq!(out, data, "zlib outbound");

        let mut e = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::new(6));
        e.write_all(&data).unwrap();
        let ref_packed = e.finish().unwrap();
        assert_eq!(
            decompress(Algorithm::Zlib, &ref_packed).unwrap(),
            data,
            "zlib inbound"
        );
    }
}

// ---- brotli (pure-Rust brotli crate) --------------------------------------

#[test]
fn brotli_both_ways() {
    if !Algorithm::Brotli.available() {
        return;
    }
    for data in samples() {
        // outbound: our compress → brotli::Decompressor
        let packed = compress(Algorithm::Brotli, &data, 6).unwrap();
        let mut d = brotli::Decompressor::new(&packed[..], 4096);
        let mut out = Vec::new();
        d.read_to_end(&mut out)
            .expect("brotli crate decode our stream");
        assert_eq!(out, data, "brotli outbound");

        // inbound: brotli::CompressorReader → our decompress
        let mut c = brotli::CompressorReader::new(&data[..], 4096, 6, 22);
        let mut ref_packed = Vec::new();
        c.read_to_end(&mut ref_packed).unwrap();
        assert_eq!(
            decompress(Algorithm::Brotli, &ref_packed).unwrap(),
            data,
            "brotli inbound"
        );
    }
}

// ---- lz4 frame (lz4_flex) -------------------------------------------------

#[test]
fn lz4_both_ways_lz4flex() {
    if !Algorithm::Lz4.available() {
        return;
    }
    for data in samples() {
        let packed = compress(Algorithm::Lz4, &data, 6).unwrap();
        let mut d = lz4_flex::frame::FrameDecoder::new(&packed[..]);
        let mut out = Vec::new();
        d.read_to_end(&mut out).expect("lz4_flex decode our frame");
        assert_eq!(out, data, "lz4 outbound");

        let mut e = lz4_flex::frame::FrameEncoder::new(Vec::new());
        e.write_all(&data).unwrap();
        let ref_packed = e.finish().unwrap();
        assert_eq!(
            decompress(Algorithm::Lz4, &ref_packed).unwrap(),
            data,
            "lz4 inbound"
        );
    }
}

// ---- snappy raw block (snap) ----------------------------------------------

#[test]
fn snappy_both_ways_snap() {
    if !Algorithm::Snappy.available() {
        return;
    }
    for data in samples() {
        let packed = compress(Algorithm::Snappy, &data, 5).unwrap();
        let out = snap::raw::Decoder::new()
            .decompress_vec(&packed)
            .expect("snap decode our raw block");
        assert_eq!(out, data, "snappy outbound");

        let ref_packed = snap::raw::Encoder::new().compress_vec(&data).unwrap();
        assert_eq!(
            decompress(Algorithm::Snappy, &ref_packed).unwrap(),
            data,
            "snappy inbound"
        );
    }
}

// ---- xz (lzma-rs) ---------------------------------------------------------

#[test]
fn xz_both_ways_lzmars() {
    if !Algorithm::Xz.available() {
        return;
    }
    for data in samples() {
        // outbound: our compress → lzma_rs::xz_decompress
        let packed = compress(Algorithm::Xz, &data, 6).unwrap();
        let mut out = Vec::new();
        lzma_rs::xz_decompress(&mut &packed[..], &mut out).expect("lzma-rs decode our xz");
        assert_eq!(out, data, "xz outbound");

        // inbound: lzma_rs::xz_compress → our decompress
        let mut ref_packed = Vec::new();
        lzma_rs::xz_compress(&mut &data[..], &mut ref_packed).unwrap();
        assert_eq!(
            decompress(Algorithm::Xz, &ref_packed).unwrap(),
            data,
            "xz inbound"
        );
    }
}
