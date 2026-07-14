//! Build script for the compress-utils Rust binding.
//!
//! Compiles the C core + every codec straight from the vendored `third_party/`
//! tree with the `cc` crate, driven by `third_party/manifest.json` so it can
//! never drift from the CMake build. Needs only a C/C++ compiler
//!
//! Each manifest codec is compiled in its **own** `cc::Build` because their
//! defines conflict (zstd needs `XXH_NAMESPACE=ZSTD_`, lz4 needs `=LZ4_`) — cc
//! defines are per-Build, not per-file. Our core + vtables compile in one more
//! Build that sees every codec's public include dirs.

use std::env;
use std::path::PathBuf;

/// Manifest codecs to compile (each becomes its own static archive). `gzip`
/// is not here — it reuses the zlib sources; only its vtable is added below.
const CODECS: &[&str] = &["zstd", "brotli", "zlib", "bz2", "lz4", "xz", "snappy"];

/// Our C core: the ABI dispatcher + registry.
const CORE_SOURCES: &[&str] = &["src/compress_utils.c", "src/registry.c"];

/// Per-algorithm vtables: (INCLUDE_<ALGO> define, vtable source). All are
/// compiled and enabled, matching the CMake defaults (every INCLUDE_* ON).
const VTABLES: &[(&str, &str)] = &[
    ("INCLUDE_ZSTD", "src/algorithms/zstd/zstd.c"),
    ("INCLUDE_BROTLI", "src/algorithms/brotli/brotli.c"),
    ("INCLUDE_ZLIB", "src/algorithms/zlib/zlib.c"),
    ("INCLUDE_GZIP", "src/algorithms/gzip/gzip.c"),
    ("INCLUDE_BZ2", "src/algorithms/bz2/bz2.c"),
    ("INCLUDE_LZ4", "src/algorithms/lz4/lz4.c"),
    ("INCLUDE_XZ", "src/algorithms/xz/xz.c"),
    ("INCLUDE_SNAPPY", "src/algorithms/snappy/snappy.c"),
];

fn main() {
    // CARGO_MANIFEST_DIR is the repo root — the manifest lives there so
    // third_party/ is reachable (see docs/adding-a-language.md).
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());

    println!("cargo:rerun-if-changed=include/compress_utils.h");
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-changed=third_party/manifest.json");
    println!("cargo:rerun-if-changed=bindings/rust/build.rs");

    let manifest: serde_json::Value = {
        let text = std::fs::read_to_string(root.join("third_party/manifest.json"))
            .expect("read third_party/manifest.json");
        serde_json::from_str(&text).expect("parse manifest.json")
    };
    let codecs = &manifest["codecs"];

    // Collect codec public include dirs so the core build (which compiles the
    // vtables that #include <zstd.h>, snappy-c.h, ...) can find them.
    let mut core_includes: Vec<PathBuf> = vec![root.join("include"), root.join("src")];

    for &name in CODECS {
        let c = &codecs[name];
        let dir = root.join("third_party").join(name);
        let cxx = c["cxx"].as_bool().unwrap_or(false);

        let mut b = cc::Build::new();
        b.cpp(cxx);
        b.warnings(false);

        for s in c["sources"].as_array().expect("sources array") {
            b.file(dir.join(s.as_str().unwrap()));
        }
        for inc in c["include_dirs"].as_array().expect("include_dirs array") {
            let p = dir.join(inc.as_str().unwrap());
            b.include(&p);
            core_includes.push(p);
        }
        for d in c["defines"].as_array().expect("defines array") {
            let (k, v) = split_define(d.as_str().unwrap());
            b.define(k, v);
        }
        // liblzma's public headers pick static-linkage decoration off this.
        if name == "xz" {
            b.define("LZMA_API_STATIC", None);
        }
        b.compile(&format!("cu_codec_{name}"));
    }

    // Core dispatcher + registry + every vtable, in one Build.
    let mut core = cc::Build::new();
    core.warnings(false);
    for inc in &core_includes {
        core.include(inc);
    }
    for s in CORE_SOURCES {
        core.file(root.join(s));
    }
    for (def, vtable) in VTABLES {
        core.define(def, None);
        core.file(root.join(vtable));
    }
    // xz.c includes <lzma.h>; needs the static-API decoration too.
    core.define("LZMA_API_STATIC", None);
    // Report the crate version from cu_version() — sidesteps the source-install
    // "header macros rot" gotcha the Go binding hit.
    let version = env::var("CARGO_PKG_VERSION").unwrap();
    core.define("CU_BUILD_VERSION", Some(format!("\"{version}\"").as_str()));
    core.compile("compress_utils_core");

    // Runtime libs the archives reference. cc emits the C++ stdlib link for the
    // snappy (cpp) Build automatically; libm (glibc) is on us.
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "linux" || target_os == "android" {
        println!("cargo:rustc-link-lib=m");
    }
}

/// Split a manifest define string ("KEY", "KEY=VAL", or `KEY="quoted val"`)
/// into the (name, value) pair `cc::Build::define` expects.
fn split_define(d: &str) -> (&str, Option<&str>) {
    match d.split_once('=') {
        Some((k, v)) => (k, Some(v)),
        None => (d, None),
    }
}
