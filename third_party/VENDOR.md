# Vendored upstream codec sources

This directory holds **curated, pre-configured** copies of the upstream
compression libraries — only the `.c`/`.cc`/headers each static library
compiles, with a portable config that needs **no configure step**. Every
language binding compiles these directly with its own toolchain (cgo, CMake,
cc-rs, SwiftPM, zig); there is no per-codec network fetch or `ExternalProject`
at build time.

## Layout

```
third_party/
  manifest.json       # single source of truth: per-codec sources, include
                      # dirs, defines, cxx flag, upstream tag, tree hash
  <codec>/            # curated upstream sources, upstream relative layout
```

`manifest.json` is read by the CMake build (`cmake/Vendor.cmake`) and is the
contract future bindings consume. `gzip` has no directory — it is zlib in a
different wire format and reuses `third_party/zlib`.

## Provenance & regeneration

Upstream versions are pinned in the repo-root [`codec-versions.json`](../codec-versions.json).
Regenerate the tree from those tags with:

```sh
tools/vendor-codecs.py                 # download the pinned tags and re-curate
tools/vendor-codecs.py --from-checkout # curate from an existing CMake checkout
tools/vendor-codecs.py --check         # CI: verify the tree matches the manifest
```

To bump a codec: edit `codec-versions.json`, run `tools/vendor-codecs.py`,
rebuild + test, and commit the refreshed `third_party/` + `manifest.json`.

## Hand-authored config (not fetched)

The upstream configure step produces target-specific headers. We replace it
with portable, compile-time-detected headers that are correct for every target
(x86-64, arm64, wasm32, Windows). `vendor-codecs.py` never overwrites these:

- `zlib/zconf.h` — a portability-patched copy of upstream's header. Upstream
  ships no header that works everywhere: the CMake-generated one hard-codes
  `#define Z_HAVE_UNISTD_H` (breaks MSVC, which has no `<unistd.h>`), while the
  pristine `zconf.h.in` leaves it off (breaks compiling zlib's `gz*.c` on POSIX,
  which need `<unistd.h>`). Our copy detects `<unistd.h>` at compile time via
  `__has_include`, so one header is correct on every target and toolchain.
- `snappy/config.h`, `snappy/snappy-stubs-public.h` — hand-authored; SIMD,
  endianness, and platform knobs resolve via `__has_builtin` / `__ARM_NEON` /
  `__SSSE3__` / `__BYTE_ORDER__` / `_WIN32` at compile time.

> These files are **preserved** across re-vendors (`vendor-codecs.py` never
> overwrites them) — which also means a version bump does **not** refresh them.
> After bumping a codec, diff each against the new upstream template. The tool
> prints a reminder listing them.

xz/liblzma needs no config header — its `HAVE_*` knobs are passed as uniform
`-D` defines (see the `xz` entry in `manifest.json`); platform-specific macros
are dropped in favor of liblzma's portable fallbacks, and threads are off.

## Licenses

Each codec retains its upstream license (see the files within each directory
and [`../ACKNOWLEDGMENTS.md`](../ACKNOWLEDGMENTS.md)). Vendoring copies source
verbatim; no upstream code is modified.
