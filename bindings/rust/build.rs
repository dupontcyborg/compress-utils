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
const CODECS: &[&str] = &["zstd", "brotli", "zlib", "bz2", "lz4", "xz"];

/// Our C core: the ABI dispatcher + registry.
const CORE_SOURCES: &[&str] = &["src/compress_utils.c", "src/registry.c"];

/// Per-algorithm vtables: (INCLUDE_<ALGO> define, vtable source). All are
/// compiled and enabled, matching the CMake defaults (every INCLUDE_* ON).
const VTABLES: &[(&str, &str)] = &[
    ("INCLUDE_ZSTD", "src/algorithms/zstd/zstd.c"),
    ("INCLUDE_BROTLI", "src/algorithms/brotli/brotli.c"),
    ("INCLUDE_ZLIB", "src/algorithms/zlib/zlib.c"),
    ("INCLUDE_DEFLATE", "src/algorithms/deflate/deflate.c"),
    ("INCLUDE_GZIP", "src/algorithms/gzip/gzip.c"),
    ("INCLUDE_BZ2", "src/algorithms/bz2/bz2.c"),
    ("INCLUDE_LZ4", "src/algorithms/lz4/lz4.c"),
    ("INCLUDE_LZ4_RAW", "src/algorithms/lz4_raw/lz4_raw.c"),
    ("INCLUDE_XZ", "src/algorithms/xz/xz.c"),
    ("INCLUDE_SNAPPY", "src/algorithms/snappy/snappy.c"),
    ("INCLUDE_LZO", "src/algorithms/lzo/lzo.c"),
];

/// Hand-ported and first-party codecs outside the manifest (see
/// third_party/VENDOR.md). They need no private defines, so they compile in the
/// core Build.
const HAND_PORTED: &[(&str, &str)] = &[
    ("third_party/lzokay", "lzokay.c"),
    ("src/algorithms/snappy/core", "snappy_core.c"),
];

fn main() {
    // CARGO_MANIFEST_DIR is the repo root — the manifest lives there so
    // third_party/ is reachable (see docs/adding-a-language.md).
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());

    println!("cargo:rerun-if-changed=include/compress_utils.h");
    println!("cargo:rerun-if-changed=src");
    println!("cargo:rerun-if-changed=third_party/manifest.json");
    println!("cargo:rerun-if-changed=third_party/lzokay");
    println!("cargo:rerun-if-changed=src/algorithms/snappy/core");
    println!("cargo:rerun-if-changed=bindings/rust/build.rs");

    let manifest: serde_json::Value = {
        let text = std::fs::read_to_string(root.join("third_party/manifest.json"))
            .expect("read third_party/manifest.json");
        serde_json::from_str(&text).expect("parse manifest.json")
    };
    let codecs = &manifest["codecs"];

    // Collect codec public include dirs so the core build (which compiles the
    // vtables that #include <zstd.h>, ...) can find them.
    let mut core_includes: Vec<PathBuf> = vec![root.join("include"), root.join("src")];

    for &name in CODECS {
        let c = &codecs[name];
        let dir = root.join("third_party").join(name);
        let cxx = c["cxx"].as_bool().unwrap_or(false);

        let mut b = cc::Build::new();
        b.cpp(cxx);
        b.warnings(false);
        // cc doesn't define NDEBUG (CMake's Release does); without it the codecs'
        // assert()s stay live (measurable on xz).
        b.define("NDEBUG", None);

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
            // CLMUL CRC64/CRC32 on x86-64, picked at run time by CPUID. The
            // manifest's define set is portable across targets and this one
            // pulls in x86 intrinsics. See algorithms/xz/CMakeLists.txt.
            let cc = b.get_compiler();
            if env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("x86_64")
                && (cc.is_like_gnu() || cc.is_like_clang())
            {
                b.define("HAVE_USABLE_CLMUL", None);
            }
        }
        b.compile(&format!("cu_codec_{name}"));
    }

    // Core dispatcher + registry + every vtable, in one Build.
    let mut core = cc::Build::new();
    core.warnings(false);
    core.define("NDEBUG", None);
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
    for (dir, source) in HAND_PORTED {
        core.include(root.join(dir));
        core.file(root.join(dir).join(source));
    }
    // xz.c includes <lzma.h>; needs the static-API decoration too.
    core.define("LZMA_API_STATIC", None);
    // Report the crate version from cu_version() — sidesteps the source-install
    // "header macros rot" gotcha the Go binding hit.
    let version = env::var("CARGO_PKG_VERSION").unwrap();
    core.define("CU_BUILD_VERSION", Some(format!("\"{version}\"").as_str()));
    core.compile("compress_utils_core");

    // Runtime libs the archives reference. Every codec is C, so no C++ stdlib
    // link is emitted; libm (glibc) is on us.
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "linux" || target_os == "android" {
        println!("cargo:rustc-link-lib=m");
    }
}

/// Optimized builds define NDEBUG, as CMake's Release configuration and the
/// upstream release builds do. `cc` does not, and liblzma keeps `assert`s in its
/// decoder's hot paths: left on, they cost over a quarter of xz decompress speed.
fn define_ndebug(b: &mut cc::Build) {
    if env::var("OPT_LEVEL").as_deref() != Ok("0") {
        b.define("NDEBUG", None);
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
