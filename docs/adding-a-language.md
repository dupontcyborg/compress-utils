# Adding a new language binding

This is the end-to-end guide for adding a language binding to compress-utils.
The C ABI in [`include/compress_utils.h`](../include/compress_utils.h) is the
canonical surface; every binding is a shim over it. What differs per language is
**how the binding gets the compiled code** — and that splits cleanly into two
groups that determine everything else (repo layout, install command, CI).

Go (added 2026-07) is the worked example throughout — grep the tree for
`bindings/go` and `gen-go-cgo` to see every touch point.

## The two groups

```
                        does the consumer install a PREBUILT artifact,
                        or COMPILE from source on their machine?
                                        │
        ┌───────────────────────────────┴───────────────────────────────┐
   Group A: prebuilt                                          Group B: source-install
   (registry ships a binary)                                 (registry/repo ships source)
        │                                                                │
   Python (wheel, PyPI)                                        Go   (go get → git repo)
   JS/WASM (npm)                                               Rust (crates.io snapshot)
   Java (JAR, Maven)                                           Swift (SwiftPM → git repo)
   CLI (release binary)                                        Zig  (zig fetch tarball)
```

- **Group A** builds a binary in CI and publishes it; the consumer's machine
  never compiles C and never sees the repo layout. These bindings live entirely
  under `bindings/<lang>/` and are built from there by CI. Nothing about their
  packaging touches the repo root.

- **Group B** ships *source*; the consumer's toolchain compiles it (with only a
  C compiler — see below). These ecosystems have rules about **where the package
  manifest must live** and **what source it can reach**, which is what drives the
  repo-layout convention.

## Repo-layout convention (Group B)

Source-install ecosystems can't reach files outside their package/module
boundary, and several *require* their manifest at the repository root:

| Language | Manifest        | Root required? | Why |
|----------|-----------------|----------------|-----|
| Swift    | `Package.swift` | **Yes (hard)** | SwiftPM cannot consume a package from a subdirectory of a repo. |
| Go       | `go.mod`        | **Yes**        | cgo only compiles C inside the module, and `go get` of a subdir module fetches only that subtree — a nested `go.mod` can't reach `third_party/`. |
| Zig      | `build.zig(.zon)` | Strongly preferred | `zig fetch` expects them at the fetched root. |
| Rust     | `Cargo.toml`    | Yes (by convention here) | `cargo publish` can't include files above the crate dir, so the crate must be rooted where `third_party/` is reachable. |

These filenames don't collide, so they coexist at the root. **The rule:**

> Source-install bindings put a *thin* manifest at the repo root and keep all
> real code under `bindings/<lang>/`, referencing `third_party/` by relative
> path. Prebuilt bindings (Group A) stay entirely under `bindings/<lang>/` and
> publish from CI. Do **not** move a Group A manifest (`pyproject.toml`,
> `package.json`) to the root — it's unnecessary and makes tools treat the whole
> repo as that ecosystem's project.

## Compiling `third_party/` from a source-install binding

The vendored codec sources live in [`third_party/`](../third_party/) with a
manifest ([`third_party/manifest.json`](../third_party/manifest.json)) giving
each codec's sources, include dirs, and defines. A Group B binding compiles them
with its *own* toolchain:

- **Go** → cgo. But cgo has two constraints that shape the solution: it only
  compiles C/C++ **in the package directory** (not `third_party/`), and its
  flags are **global to the package** (can't set per-file macros, yet zstd needs
  `XXH_NAMESPACE=ZSTD_` while lz4 needs `=LZ4_`). Both are solved by generated
  **shim translation units**: [`tools/gen-go-cgo.py`](../tools/gen-go-cgo.py)
  emits one `bindings/go/cu_gen_*.c` per vendored source that `#define`s that
  codec's macros and `#include`s the real file by relative path. The shims are
  committed (so `go get` needs no generator — just a C compiler) and CI
  drift-checks them against the manifest.
- **Rust** (added 2026-07) → the `cc` crate in `build.rs` reads the manifest and
  compiles each codec in its own `cc::Build` (per-Build defines, so no shims —
  unlike Go, `cc` isolates the conflicting `XXH_NAMESPACE` etc.). Vendored-from-
  source only, like every `-sys`-style crate: a prebuilt-download path was
  prototyped and dropped — it tripled the dependency graph (a TLS/HTTP stack) to
  skip a ~1 s Cargo-cached compile and broke offline/docs.rs builds. (The clean
  static `.a` is still published to Releases, for C/C++/FFI consumers — just not
  fetched by the crate.) FFI is hand-written (no bindgen). `build.rs` injects
  `CU_BUILD_VERSION` from `CARGO_PKG_VERSION`, so — unlike Go — the source build
  reports the right version without touching the header macros (sync-versions
  still keeps `Cargo.toml` itself in step).
- **Zig** → `build.zig` `addCSourceFiles` with the manifest's sources/flags;
  `zig cc` also cross-compiles.
- **Swift** → a SwiftPM C target with `path:`/`cSettings` pointing at
  `third_party/` + the vtables.

Every one of these needs only a **C compiler** on the consumer's machine — never
CMake, never the network (beyond the package fetch). That is the entire payoff
of vendoring.

> **cgo filename trap:** Go applies build constraints from filename suffixes —
> a file ending `_arm.c`, `_riscv.c`, `_sparc.c` (all recognized GOARCH tokens,
> and all present in xz's `simple/` sources) is silently excluded on other
> targets. The generator appends a constant `_cgen` suffix to every shim so no
> filename ends in an arch/OS token. Don't remove it.

## Step-by-step (Group B, using Go)

- [ ] **Root manifest.** Add the thin manifest at the repo root
      (`go.mod`: `module github.com/dupontcyborg/compress-utils`, `go 1.21`).
      Do not put it under `bindings/<lang>/`.
- [ ] **Binding source** under `bindings/<lang>/`: the idiomatic wrapper over the
      C ABI. For Go that's `compress_utils.go` (one-shot + error/enum mapping,
      and the hand-written cgo preamble with runtime link flags: `-lc++`/`-lstdc++`
      for snappy, `-lm` on glibc), `stream.go` (`io.Reader`/`io.Writer`), `doc.go`.
      Make the language's idioms feel native — generators/exceptions/`io`
      interfaces — over the uniform C substrate.
- [ ] **Source-compile plumbing** for `third_party/`: for Go, the generator
      `tools/gen-go-cgo.py` (emits `cu_gen_*.c` shims + `cgo_generated.go` with
      the `-I`/`-DINCLUDE_*` flags). Run it and **commit** the output.
- [ ] **Tests** (`bindings/<lang>/*_test.*`): round-trip and streaming for every
      available algorithm, cross-API (stream output decodes one-shot and vice
      versa — catches wire-format mismatches), garbage rejection, and interop
      against the language's *own* independent codecs (Go uses stdlib
      `compress/gzip`, `compress/zlib`, `compress/bzip2`).
- [ ] **`build.sh`** — add the language to `--languages` dispatch (a `WANT_<L>`
      flag + a build path). Source-install bindings don't set `NEEDS_CMAKE`.
- [ ] **CI** — a `.github/workflows/build_and_test_<lang>.yml` that (a) runs the
      shim/source drift-check and (b) builds + tests on the platforms whose
      toolchain is available (Go: ubuntu + macOS; Windows cgo needs mingw setup,
      deferred).
- [ ] **Docs** — bump the `languages-N` README badge, add a row to the
      **Supported languages** table, and tick the binding in `TODO.md`.

## Verifying

```bash
# Go: build + full test suite (needs a C compiler; cgo compiles third_party once)
CGO_ENABLED=1 go test ./bindings/go/
gofmt -l bindings/go/*.go            # formatting (empty = clean)
python3 tools/gen-go-cgo.py --check  # shims match the manifest

# Or via the orchestrator:
./build.sh --languages=go
```

A clean checkout with only a C compiler + the language toolchain must build and
pass — no CMake, no prebuilt library, no network.
