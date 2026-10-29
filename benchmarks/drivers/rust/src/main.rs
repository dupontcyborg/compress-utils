//! Rust benchmark driver, for compress-utils and for the Rust libraries it competes with.
//!
//! Speaks the shared benchmark driver protocol (see benchmarks/README.md): reads
//! "<algo> <level> [<mode>] <path>" job lines from stdin (mode: oneshot, stream or
//! msgset), emits one NDJSON result (or skip/error marker) per line, honours
//! BENCH_SAMPLES / BENCH_WARMUP / BENCH_CHUNK, and answers `--info`.
//!
//! `--impl` picks the implementation (default compress-utils); every one goes through
//! the same timing loop, as a consumer would call it (src/impls.rs):
//!
//!   oneshot  compress, then decompress without being told the size
//!   msgset   one compress call per message, each decompressed without being told its size
//!   stream   the input fed in BENCH_CHUNK pieces through the library's streaming
//!            API, decompressed output counted and discarded; skipped when it has none
//!
//! Competitors run at native levels (the runner translates through levels.json, or
//! sweeps the range each codec declares) and are set to compress-utils' wire format
//! where their API allows. Each competitor's output is also decoded by compress-utils
//! once per job, untimed, and the record carries the result as `interop`.
//! compress-utils compiles its C core from source via its build script, so it runs the
//! same codec code as the other drivers; only the language wrapper differs.

mod impls;

use std::env;
use std::fmt::Write as _;
use std::io::{self, BufRead, Write};
use std::time::Instant;

use impls::{Codec, ALGOS};

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

/// Runs `f` warmup + samples times; returns the last result and the timed runs.
fn timed<T>(
    samples: usize,
    warmup: usize,
    mut f: impl FnMut() -> io::Result<T>,
) -> io::Result<(T, Vec<u128>)> {
    let mut times = Vec::with_capacity(samples);
    let mut out = None;
    for r in 0..warmup + samples {
        let t0 = Instant::now();
        out = Some(f()?);
        if r >= warmup {
            times.push(t0.elapsed().as_nanos());
        }
    }
    Ok((out.expect("samples > 0"), times))
}

struct Job<'a> {
    impl_name: &'a str,
    algo: &'a str,
    codec: &'a Codec,
    level: i32,
    samples: usize,
    warmup: usize,
    chunk: usize,
}

impl Job<'_> {
    /// compress-utils' own one-shot compress, for decode-only inputs and interop checks.
    fn cu_algo(&self) -> compress_utils::Algorithm {
        ALGOS.iter().find(|(n, _)| *n == self.algo).expect("known algo").1
    }

    /// Can compress-utils decode this library's output? None for compress-utils itself.
    fn interop(&self, comp: &[u8], data: &[u8]) -> Option<bool> {
        (self.impl_name != "compress-utils").then(|| {
            compress_utils::decompress(self.cu_algo(), comp).is_ok_and(|d| d == data)
        })
    }

    fn record(
        &self,
        mode: &str,
        path: &str,
        in_bytes: usize,
        out_bytes: usize,
        c_t: &[u128],
        d_t: &[u128],
        verified: bool,
        interop: Option<bool>,
        messages: Option<usize>,
    ) -> String {
        let (c, d) = (stats(c_t), stats(d_t));
        let chunk_bytes = if mode == "stream" { self.chunk } else { 0 };
        // Hand-rolled JSON: a flat object of numbers plus a few strings, not worth a
        // serde dependency in a driver whose whole job is to not perturb the measurement.
        let mut s = String::new();
        write!(
            s,
            concat!(
                r#"{{"lang":"rust","impl":{},"algo":{},"level":{},"mode":"{}","chunk_bytes":{},"#,
                r#""input":{},"input_bytes":{},"output_bytes":{},"#,
                r#""compress_ns_median":{},"compress_ns_mad":{},"compress_ns_min":{},"#,
                r#""decompress_ns_median":{},"decompress_ns_mad":{},"decompress_ns_min":{},"#,
                r#""samples":{},"warmup":{},"verified":{}"#,
            ),
            json_string(self.impl_name),
            json_string(self.algo),
            self.level,
            mode,
            chunk_bytes,
            json_string(path),
            in_bytes,
            out_bytes,
            c.median,
            c.mad,
            c.min,
            d.median,
            d.mad,
            d.min,
            self.samples,
            self.warmup,
            verified,
        )
        .expect("writing to a String cannot fail");
        if let Some(n) = messages {
            write!(s, r#","messages":{n},"decompress_size_known":false"#).unwrap();
        }
        if let Some(ok) = interop {
            write!(s, r#","interop":{ok}"#).unwrap();
        }
        if self.codec.compress.is_none() {
            s.push_str(r#","decode_only":true"#);
        }
        s.push('}');
        s
    }

    fn run(&self, mode: &str, path: &str) -> io::Result<String> {
        let data = std::fs::read(path)?;
        let stream = mode == "stream";
        let (comp, c_t) = match &self.codec.compress {
            // Decode-only library: its input comes from compress-utils, untimed.
            None => (
                compress_utils::compress(self.cu_algo(), &data, 9).map_err(io::Error::from)?,
                vec![0; self.samples],
            ),
            Some(enc) => {
                let enc = if stream { self.codec.cstream.as_ref().unwrap() } else { enc };
                timed(self.samples, self.warmup, || enc(&data, self.level))?
            }
        };
        let (dec, d_t) = if stream {
            let dstream = self.codec.dstream.as_ref().unwrap();
            let (_, d_t) = timed(self.samples, self.warmup, || {
                let mut sink = CountWriter(0);
                dstream(&comp, &mut sink)?;
                Ok(sink.0)
            })?;
            let mut dec = Vec::with_capacity(data.len()); // untimed, for verification
            dstream(&comp, &mut dec)?;
            (dec, d_t)
        } else {
            timed(self.samples, self.warmup, || (self.codec.decompress)(&comp))?
        };
        Ok(self.record(mode, path, data.len(), comp.len(), &c_t, &d_t, dec == data,
                       self.interop(&comp, &data), None))
    }

    fn run_msgset(&self, path: &str) -> io::Result<String> {
        let msgs = read_msgset(path)?;
        let (comp, c_t): (Vec<Vec<u8>>, Vec<u128>) = match &self.codec.compress {
            None => (
                msgs.iter()
                    .map(|m| compress_utils::compress(self.cu_algo(), m, 9))
                    .collect::<Result<_, _>>()
                    .map_err(io::Error::from)?,
                vec![0; self.samples],
            ),
            Some(enc) => timed(self.samples, self.warmup, || {
                msgs.iter().map(|m| enc(m, self.level)).collect::<io::Result<Vec<_>>>()
            })?,
        };
        let ((), d_t) = timed(self.samples, self.warmup, || {
            for c in &comp {
                (self.codec.decompress)(c)?;
            }
            Ok(())
        })?;
        let verified = comp.iter().zip(&msgs).all(|(c, m)| {
            (self.codec.decompress)(c).is_ok_and(|d| d == *m)
        });
        let interop = msgs.first().and_then(|m| self.interop(&comp[0], m));
        Ok(self.record(
            "msgset",
            path,
            msgs.iter().map(Vec::len).sum(),
            comp.iter().map(Vec::len).sum(),
            &c_t,
            &d_t,
            verified,
            interop,
            Some(msgs.len()),
        ))
    }
}

/// Counts bytes and discards them.
struct CountWriter(usize);

impl Write for CountWriter {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.0 += buf.len();
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Reads a message set: records of a u32 little-endian length followed by that many bytes.
fn read_msgset(path: &str) -> io::Result<Vec<Vec<u8>>> {
    let data = std::fs::read(path)?;
    let bad = || io::Error::new(io::ErrorKind::InvalidData, "truncated message set");
    let mut msgs = Vec::new();
    let mut pos = 0;
    while pos < data.len() {
        let len = data.get(pos..pos + 4).ok_or_else(bad)?;
        let n = u32::from_le_bytes(len.try_into().unwrap()) as usize;
        pos += 4;
        msgs.push(data.get(pos..pos + n).ok_or_else(bad)?.to_vec());
        pos += n;
    }
    Ok(msgs)
}

/// Minimal JSON string escaping — corpus paths are plain filesystem paths, but a stray
/// backslash or quote must not produce a line the runner can't parse.
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

fn info(impl_name: &str, im: &impls::Impl) -> String {
    let levels: Vec<String> = im
        .codecs
        .iter()
        .filter_map(|(a, c)| c.levels.map(|(lo, hi)| format!("{}:[{lo},{hi}]", json_string(a))))
        .collect();
    let levelless: Vec<String> =
        im.codecs.iter().filter(|(_, c)| c.levelless).map(|(a, _)| json_string(a)).collect();
    let driver = if impl_name == "compress-utils" { "rust".to_string() } else { format!("rust-{impl_name}") };
    format!(
        r#"{{"lang":"rust","version":{},"driver":{},"impl":{},"levels":{{{}}},"levelless":[{}]}}"#,
        json_string(&im.version),
        json_string(&driver),
        json_string(impl_name),
        levels.join(","),
        levelless.join(",")
    )
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let impl_name = args
        .iter()
        .position(|a| a == "--impl")
        .and_then(|i| args.get(i + 1))
        .map_or("compress-utils", String::as_str);
    let Some(im) = impls::load(impl_name) else {
        eprintln!("bench-rust: unknown impl {impl_name:?} (known: {})", impls::NAMES.join(", "));
        std::process::exit(2);
    };
    if args.iter().any(|a| a == "--info") {
        emit(&info(impl_name, &im));
        return;
    }

    let samples = env_usize("BENCH_SAMPLES", 5);
    let warmup = env_usize("BENCH_WARMUP", 1);
    let chunk = env_usize("BENCH_CHUNK", 64 * 1024);
    impls::CHUNK.set(chunk).unwrap();

    for line in io::stdin().lock().lines() {
        let Ok(line) = line else { break };
        let line = line.trim();
        if line.is_empty() {
            continue;
        }

        // "<algo> <level> [<mode>] <path>"; path may contain spaces.
        let mut parts = line.splitn(3, ' ');
        let (algo, level_s, rest) = match (parts.next(), parts.next(), parts.next()) {
            (Some(a), Some(l), Some(r)) => (a, l, r),
            _ => {
                emit(r#"{"error":true}"#);
                continue;
            }
        };
        let (mode, rest) = ["stream", "msgset", "oneshot"]
            .iter()
            .find_map(|m| rest.strip_prefix(m).and_then(|r| r.strip_prefix(' ')).map(|r| (*m, r)))
            .unwrap_or(("oneshot", rest));
        let path = rest.trim();

        let Some(codec) = im.codecs.get(algo) else {
            emit(r#"{"skipped":true}"#);
            continue;
        };
        if mode == "stream"
            && (codec.dstream.is_none() || (codec.cstream.is_none() && codec.compress.is_some()))
        {
            emit(r#"{"skipped":true}"#);
            continue;
        }
        let Ok(level) = level_s.parse::<i32>() else {
            emit(r#"{"error":true}"#);
            continue;
        };

        let job = Job { impl_name, algo, codec, level, samples, warmup, chunk };
        let result = if mode == "msgset" { job.run_msgset(path) } else { job.run(mode, path) };
        match result {
            Ok(rec) => emit(&rec),
            Err(e) => {
                eprintln!("bench-rust[{impl_name}]: {algo} L{level} {mode} failed: {e}");
                emit(r#"{"error":true}"#);
            }
        }
    }
}
