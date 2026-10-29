//! The implementations the driver can benchmark: compress-utils and the Rust
//! libraries it competes with. Each is a set of `Codec`s, one per format.

use std::collections::BTreeMap;
use std::io::{self, Read, Write};

use compress_utils::{
    compress, decompress, Algorithm, Compressor, Decompressor,
};

/// Streaming and one-shot chunk size; set once by `main` from BENCH_CHUNK.
pub static CHUNK: std::sync::OnceLock<usize> = std::sync::OnceLock::new();

fn chunk() -> usize {
    *CHUNK.get().unwrap_or(&(64 * 1024))
}

type CompressFn = Box<dyn Fn(&[u8], i32) -> io::Result<Vec<u8>>>;
/// Never told the decompressed size.
type DecompressFn = Box<dyn Fn(&[u8]) -> io::Result<Vec<u8>>>;
/// Decodes through the streaming API into the writer, in reads of a fixed window.
type DStreamFn = Box<dyn Fn(&[u8], &mut dyn Write) -> io::Result<()>>;

/// One implementation of one format. A missing `compress` marks a decode-only
/// library (the driver then produces its input with compress-utils, untimed).
pub struct Codec {
    pub compress: Option<CompressFn>,
    pub decompress: DecompressFn,
    pub cstream: Option<CompressFn>,
    pub dstream: Option<DStreamFn>,
    /// Native level range; None = levels.json's.
    pub levels: Option<(i32, i32)>,
    pub levelless: bool,
}

pub struct Impl {
    pub version: String,
    pub codecs: BTreeMap<&'static str, Codec>,
}

pub const NAMES: &[&str] =
    &["compress-utils", "flate2", "flate2-zlib-rs", "zstd", "brotli", "lz4_flex", "snap", "bzip2", "xz2"];

pub fn load(name: &str) -> Option<Impl> {
    Some(match name {
        "compress-utils" => compress_utils_impl(),
        "flate2" if !cfg!(feature = "zlib-rs") => flate2_impl(),
        "flate2-zlib-rs" if cfg!(feature = "zlib-rs") => flate2_impl(),
        "zstd" => zstd_impl(),
        "brotli" => brotli_impl(),
        "lz4_flex" => lz4_flex_impl(),
        "snap" => snap_impl(),
        "bzip2" => bzip2_impl(),
        "xz2" => xz2_impl(),
        _ => return None,
    })
}

fn err<E: std::fmt::Display>(e: E) -> io::Error {
    io::Error::new(io::ErrorKind::Other, e.to_string())
}

/// Feeds `data` to `w` in `chunk()`-sized writes.
fn write_chunked<W: Write>(w: &mut W, data: &[u8]) -> io::Result<()> {
    for piece in data.chunks(chunk()) {
        w.write_all(piece)?;
    }
    Ok(())
}

/// Reads a decoder to the end.
fn read_all<R: Read>(mut r: R) -> io::Result<Vec<u8>> {
    let mut out = Vec::new();
    r.read_to_end(&mut out)?;
    Ok(out)
}

/// Reads a decoder to the end through one fixed window, handing each read to `w`.
fn drain<R: Read>(mut r: R, w: &mut dyn Write) -> io::Result<()> {
    let mut buf = [0u8; 64 * 1024];
    loop {
        match r.read(&mut buf) {
            Ok(0) => return Ok(()),
            Ok(n) => w.write_all(&buf[..n])?,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
}

type MkDecoder = for<'a> fn(&'a [u8]) -> io::Result<Box<dyn Read + 'a>>;

/// A codec for a format whose library has writer/reader adapters: `mk_enc` builds an
/// encoder around a Vec, `finish` completes it, `mk_dec` builds a decoder over bytes.
/// One-shot writes the input in one call; streaming writes it in chunks.
fn adapter_codec<E>(
    mk_enc: impl Fn(Vec<u8>, i32) -> io::Result<E> + Clone + 'static,
    finish: impl Fn(E) -> io::Result<Vec<u8>> + Clone + 'static,
    mk_dec: MkDecoder,
) -> Codec
where
    E: Write + 'static,
{
    let enc = move |whole: bool| -> CompressFn {
        let (mk_enc, finish) = (mk_enc.clone(), finish.clone());
        Box::new(move |data, lvl| {
            let mut e = mk_enc(Vec::with_capacity(data.len() / 2 + 64), lvl)?;
            if whole {
                e.write_all(data)?;
            } else {
                write_chunked(&mut e, data)?;
            }
            finish(e)
        })
    };
    Codec {
        compress: Some(enc(true)),
        decompress: Box::new(move |c| read_all(mk_dec(c)?)),
        cstream: Some(enc(false)),
        dstream: Some(Box::new(move |c, w| drain(mk_dec(c)?, w))),
        levels: None,
        levelless: false,
    }
}

// ---- compress-utils --------------------------------------------------------------

fn compress_utils_impl() -> Impl {
    let codec = |algo: Algorithm| Codec {
        compress: Some(Box::new(move |d, l| compress(algo, d, l).map_err(io::Error::from))),
        decompress: Box::new(move |c| decompress(algo, c).map_err(io::Error::from)),
        cstream: Some(Box::new(move |d, l| {
            let mut enc = Compressor::new(Vec::new(), algo, l).map_err(io::Error::from)?;
            write_chunked(&mut enc, d)?;
            enc.finish()
        })),
        dstream: Some(Box::new(move |c, w| {
            let dec = Decompressor::new(c, algo).map_err(io::Error::from)?;
            drain(dec, w)
        })),
        levels: None,
        levelless: false,
    };
    let codecs = ALGOS.iter().map(|(n, a)| (*n, codec(*a))).collect();
    Impl { version: compress_utils::version().to_string(), codecs }
}

pub const ALGOS: &[(&str, Algorithm)] = &[
    ("zstd", Algorithm::Zstd),
    ("brotli", Algorithm::Brotli),
    ("zlib", Algorithm::Zlib),
    ("bz2", Algorithm::Bz2),
    ("lz4", Algorithm::Lz4),
    ("xz", Algorithm::Xz),
    ("snappy", Algorithm::Snappy),
    ("gzip", Algorithm::Gzip),
];

// ---- flate2 (miniz_oxide by default, zlib-rs with the `zlib-rs` feature) ---------

fn flate2_impl() -> Impl {
    use flate2::read::{GzDecoder, ZlibDecoder};
    use flate2::write::{GzEncoder, ZlibEncoder};
    use flate2::Compression;
    let zlib = adapter_codec(
        |w, l| Ok(ZlibEncoder::new(w, Compression::new(l as u32))),
        |e: ZlibEncoder<Vec<u8>>| e.finish(),
        |c| Ok(Box::new(ZlibDecoder::new(c))),
    );
    let gzip = adapter_codec(
        |w, l| Ok(GzEncoder::new(w, Compression::new(l as u32))),
        |e: GzEncoder<Vec<u8>>| e.finish(),
        |c| Ok(Box::new(GzDecoder::new(c))),
    );
    Impl {
        version: format!("flate2 1.1 ({})", if cfg!(feature = "zlib-rs") { "zlib-rs" } else { "miniz_oxide" }),
        codecs: [("zlib", zlib), ("gzip", gzip)].into_iter().collect(),
    }
}

// ---- zstd (bindings to libzstd) --------------------------------------------------

fn zstd_impl() -> Impl {
    let mut c = adapter_codec(
        |w, l| zstd::stream::write::Encoder::new(w, l),
        |e: zstd::stream::write::Encoder<'static, Vec<u8>>| e.finish(),
        |c| Ok(Box::new(zstd::stream::read::Decoder::new(c)?)),
    );
    // One-shot uses the bulk API, as its docs recommend; frames carry the content size.
    c.compress = Some(Box::new(|d, l| zstd::bulk::compress(d, l)));
    c.decompress = Box::new(|c| zstd::stream::decode_all(c));
    c.levels = Some((1, 22));
    Impl { version: "zstd crate 0.14 (libzstd 1.5.7)".into(), codecs: [("zstd", c)].into_iter().collect() }
}

// ---- brotli (the Rust port) ------------------------------------------------------

fn brotli_impl() -> Impl {
    const BUF: usize = 64 * 1024;
    let mut c = adapter_codec(
        |w, l| Ok(brotli::CompressorWriter::new(w, BUF, l as u32, 22)),
        |mut e: brotli::CompressorWriter<Vec<u8>>| {
            e.flush()?;
            Ok(e.into_inner())
        },
        |c| Ok(Box::new(brotli::Decompressor::new(c, BUF))),
    );
    c.levels = Some((0, 11));
    Impl { version: "brotli crate 9".into(), codecs: [("brotli", c)].into_iter().collect() }
}

// ---- lz4_flex --------------------------------------------------------------------

fn lz4_flex_impl() -> Impl {
    use lz4_flex::frame::{BlockSize, FrameDecoder, FrameEncoder, FrameInfo};
    // LZ4 frames like compress-utils': content checksum and, in one-shot, the size.
    let enc = |whole: bool| -> CompressFn {
        Box::new(move |data, _| {
            let mut info = FrameInfo::new().content_checksum(true).block_size(BlockSize::Max64KB);
            if whole {
                info = info.content_size(Some(data.len() as u64));
            }
            let mut e = FrameEncoder::with_frame_info(info, Vec::with_capacity(data.len() / 2 + 64));
            if whole {
                e.write_all(data)?;
            } else {
                write_chunked(&mut e, data)?;
            }
            e.finish().map_err(err)
        })
    };
    let c = Codec {
        compress: Some(enc(true)),
        decompress: Box::new(|c| read_all(FrameDecoder::new(c))),
        cstream: Some(enc(false)),
        dstream: Some(Box::new(|c, w| drain(FrameDecoder::new(c), w))),
        levels: None,
        levelless: true,
    };
    Impl { version: "lz4_flex 0.14".into(), codecs: [("lz4", c)].into_iter().collect() }
}

// ---- snap ------------------------------------------------------------------------

fn snap_impl() -> Impl {
    // Raw snappy blocks (compress-utils' format); the crate's stream form is the
    // framing format, a different one, so no streaming.
    let c = Codec {
        compress: Some(Box::new(|d, _| snap::raw::Encoder::new().compress_vec(d).map_err(err))),
        decompress: Box::new(|c| snap::raw::Decoder::new().decompress_vec(c).map_err(err)),
        cstream: None,
        dstream: None,
        levels: None,
        levelless: true,
    };
    Impl { version: "snap 1.1".into(), codecs: [("snappy", c)].into_iter().collect() }
}

// ---- bzip2 (libbz2-rs-sys, pure Rust) --------------------------------------------

fn bzip2_impl() -> Impl {
    use bzip2::read::BzDecoder;
    use bzip2::write::BzEncoder;
    use bzip2::Compression;
    let mut c = adapter_codec(
        |w, l| Ok(BzEncoder::new(w, Compression::new(l as u32))),
        |e: BzEncoder<Vec<u8>>| e.finish(),
        |c| Ok(Box::new(BzDecoder::new(c))),
    );
    c.levels = Some((1, 9));
    Impl { version: "bzip2 crate 0.6 (libbz2-rs-sys)".into(), codecs: [("bz2", c)].into_iter().collect() }
}

// ---- xz2 (bindings to liblzma) ---------------------------------------------------

fn xz2_impl() -> Impl {
    use xz2::read::XzDecoder;
    use xz2::write::XzEncoder;
    let mut c = adapter_codec(
        |w, l| Ok(XzEncoder::new(w, l as u32)),
        |e: XzEncoder<Vec<u8>>| e.finish(),
        |c| Ok(Box::new(XzDecoder::new(c))),
    );
    c.levels = Some((0, 9));
    Impl { version: "xz2 crate 0.1 (liblzma)".into(), codecs: [("xz", c)].into_iter().collect() }
}
