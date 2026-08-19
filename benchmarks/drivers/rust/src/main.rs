//! Rust benchmark driver.
//!
//! Speaks the shared benchmark driver protocol (see benchmarks/README.md): reads
//! "<algo> <level> [<mode>] <path>" job lines from stdin, emits one NDJSON result
//! (or skip/error marker) per line, honours BENCH_SAMPLES / BENCH_WARMUP /
//! BENCH_CHUNK, and answers `--info`.
//!
//! Drives the compress-utils Rust binding the way a consumer would:
//! `compress`/`decompress` for one-shot, `Compressor`/`Decompressor` for
//! streaming. The crate compiles the C core from source via its build script, so
//! this is the same codec code the other drivers measure — only the language
//! wrapper differs.

use std::env;
use std::fmt::Write as _;
use std::io::{self, BufRead, Read, Write};
use std::time::Instant;

use compress_utils::{compress, decompress, Algorithm, Compressor, Decompressor};

fn algo_from_name(name: &str) -> Option<Algorithm> {
    Some(match name {
        "zstd" => Algorithm::Zstd,
        "brotli" => Algorithm::Brotli,
        "zlib" => Algorithm::Zlib,
        "bz2" => Algorithm::Bz2,
        "lz4" => Algorithm::Lz4,
        "xz" => Algorithm::Xz,
        "snappy" => Algorithm::Snappy,
        "gzip" => Algorithm::Gzip,
        _ => return None,
    })
}

fn env_usize(name: &str, fallback: usize) -> usize {
    env::var(name)
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .filter(|n| *n > 0)
        .unwrap_or(fallback)
}

struct Stats {
    median: u128,
    mad: u128,
    min: u128,
}

fn median(sorted: &[u128]) -> u128 {
    let n = sorted.len();
    if n % 2 == 1 {
        sorted[n / 2]
    } else {
        (sorted[n / 2 - 1] + sorted[n / 2]) / 2
    }
}

fn stats(samples: &[u128]) -> Stats {
    let mut s = samples.to_vec();
    s.sort_unstable();
    let m = median(&s);
    let mut dev: Vec<u128> = s.iter().map(|x| if *x > m { x - m } else { m - x }).collect();
    dev.sort_unstable();
    Stats { median: m, mad: median(&dev), min: s[0] }
}

fn do_compress(
    algo: Algorithm,
    data: &[u8],
    level: i32,
    is_stream: bool,
    chunk: usize,
) -> io::Result<Vec<u8>> {
    if !is_stream {
        return compress(algo, data, level).map_err(io::Error::from);
    }
    let mut enc = Compressor::new(Vec::new(), algo, level).map_err(io::Error::from)?;
    for piece in data.chunks(chunk) {
        enc.write_all(piece)?;
    }
    enc.finish()
}

fn do_decompress(algo: Algorithm, comp: &[u8], is_stream: bool) -> io::Result<Vec<u8>> {
    if !is_stream {
        return decompress(algo, comp).map_err(io::Error::from);
    }
    let mut dec = Decompressor::new(comp, algo).map_err(io::Error::from)?;
    let mut out = Vec::new();
    dec.read_to_end(&mut out)?;
    Ok(out)
}

fn run_job(
    algo_name: &str,
    algo: Algorithm,
    level: i32,
    is_stream: bool,
    path: &str,
    samples: usize,
    warmup: usize,
    chunk: usize,
) -> io::Result<String> {
    let data = std::fs::read(path)?;

    let mut comp = Vec::new();
    for _ in 0..warmup {
        comp = do_compress(algo, &data, level, is_stream, chunk)?;
    }
    let mut c_t = Vec::with_capacity(samples);
    for _ in 0..samples {
        let t0 = Instant::now();
        comp = do_compress(algo, &data, level, is_stream, chunk)?;
        c_t.push(t0.elapsed().as_nanos());
    }

    let mut dec = Vec::new();
    for _ in 0..warmup {
        dec = do_decompress(algo, &comp, is_stream)?;
    }
    let mut d_t = Vec::with_capacity(samples);
    for _ in 0..samples {
        let t0 = Instant::now();
        dec = do_decompress(algo, &comp, is_stream)?;
        d_t.push(t0.elapsed().as_nanos());
    }

    let (c, d) = (stats(&c_t), stats(&d_t));
    let mode = if is_stream { "stream" } else { "oneshot" };
    let chunk_bytes = if is_stream { chunk } else { 0 };

    // Hand-rolled JSON: the payload is a flat object of numbers plus two paths,
    // not worth a serde dependency in a driver whose whole job is to not
    // perturb the measurement.
    let mut s = String::new();
    write!(
        s,
        concat!(
            r#"{{"lang":"rust","impl":"compress-utils","algo":{},"level":{},"#,
            r#""mode":"{}","chunk_bytes":{},"input":{},"input_bytes":{},"output_bytes":{},"#,
            r#""compress_ns_median":{},"compress_ns_mad":{},"compress_ns_min":{},"#,
            r#""decompress_ns_median":{},"decompress_ns_mad":{},"decompress_ns_min":{},"#,
            r#""samples":{},"warmup":{},"verified":{}}}"#,
        ),
        json_string(algo_name),
        level,
        mode,
        chunk_bytes,
        json_string(path),
        data.len(),
        comp.len(),
        c.median,
        c.mad,
        c.min,
        d.median,
        d.mad,
        d.min,
        samples,
        warmup,
        dec == data,
    )
    .expect("writing to a String cannot fail");
    Ok(s)
}

/// Minimal JSON string escaping — corpus paths are plain filesystem paths, but
/// a stray backslash or quote must not produce a line the runner can't parse.
fn json_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn emit(line: &str) {
    let mut stdout = io::stdout();
    let _ = writeln!(stdout, "{line}");
    let _ = stdout.flush();
}

fn main() {
    if env::args().nth(1).as_deref() == Some("--info") {
        emit(&format!(
            r#"{{"lang":"rust","version":{},"driver":"rust"}}"#,
            json_string(compress_utils::version())
        ));
        return;
    }

    let samples = env_usize("BENCH_SAMPLES", 5);
    let warmup = env_usize("BENCH_WARMUP", 1);
    let chunk = env_usize("BENCH_CHUNK", 64 * 1024);

    let stdin = io::stdin();
    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        // "<algo> <level> [<mode>] <path>"; path may contain spaces.
        let mut parts = line.splitn(3, ' ');
        let (algo_name, level_s, rest) = match (parts.next(), parts.next(), parts.next()) {
            (Some(a), Some(l), Some(r)) => (a, l, r),
            _ => {
                emit(r#"{"error":true}"#);
                continue;
            }
        };

        let (is_stream, rest) = if let Some(r) = rest.strip_prefix("stream ") {
            (true, r)
        } else if let Some(r) = rest.strip_prefix("oneshot ") {
            (false, r)
        } else {
            (false, rest)
        };
        let path = rest.trim();

        let algo = match algo_from_name(algo_name) {
            Some(a) => a,
            None => {
                emit(r#"{"skipped":true}"#);
                continue;
            }
        };
        let level: i32 = match level_s.parse() {
            Ok(l) => l,
            Err(_) => {
                emit(r#"{"error":true}"#);
                continue;
            }
        };

        match run_job(algo_name, algo, level, is_stream, path, samples, warmup, chunk) {
            Ok(rec) => emit(&rec),
            Err(e) => {
                let mode = if is_stream { "stream" } else { "oneshot" };
                eprintln!("bench-rust: {algo_name} L{level} {mode} failed: {e}");
                emit(r#"{"error":true}"#);
            }
        }
    }
}
