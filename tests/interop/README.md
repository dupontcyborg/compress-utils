# Interop tests

These tests prove that compress-utils produces and consumes the *real*
wire formats — valid `.zst` / `.br` / zlib / `.bz2` / `.lz4` / `.xz`
streams — not just streams that happen to round-trip against our own
decoder.

## Why this exists

The smoke suites (`tests/test_compress_utils.c`,
`bindings/cpp/test/test_compress_utils.cpp`,
`bindings/python/tests/test_compress_utils.py`) only prove
**self-consistency**: our compressor's output decodes with our
decompressor, including across the one-shot ↔ streaming boundary.

These interop tests fix that by round-tripping against **independent
canonical implementations**, in both directions, for every algorithm:

- **outbound** — `cu.compress(x)` → canonical decoder → must equal `x`
  (proves we emit valid streams)
- **inbound** — canonical encoder → `cu.decompress(...)` → must equal `x`
  (proves we accept streams we didn't produce)

## Two channels

### 1. Python-library channel — `bindings/python/tests/test_interop.py`

Round-trips our output against PyPI / stdlib codec libraries:

| algorithm | canonical reference   | source                     |
|-----------|-----------------------|----------------------------|
| zstd      | `zstandard`           | PyPI (own libzstd)         |
| brotli    | `brotli`              | PyPI (own libbrotli)       |
| zlib      | `zlib`                | Python stdlib (system zlib)|
| bz2       | `bz2`                 | Python stdlib (system bz2) |
| lz4       | `lz4.frame`           | PyPI (own liblz4)          |
| xz        | `lzma`                | Python stdlib (system lzma)|

The three PyPI references are optional: if one isn't installed, that
algorithm self-skips, so the suite is green on a bare `pip install
pytest`. Install them (or `pip install -e '.[test]'`) to run the full
set. This is the channel that runs across the entire wheel matrix in CI
via the cibuildwheel `test-requires`.

### 2. CLI channel — `tests/interop/cli_crosscheck.py`

Round-trips our output against the canonical reference **binaries** —
`zstd`, `xz`, `lz4`, `bzip2`, `brotli` — exercising the on-disk file
formats. This is the channel that directly mirrors how the legacy LZ4 bug
would have been caught: the `lz4` CLI is *the* canonical LZ4-frame
consumer. Drives our side through the `compress_utils` Python binding
(the project ships no standalone CLI yet).

Tools that aren't on `PATH` self-skip — it only fails on a genuine
mismatch — so it's safe to run anywhere.

> **Why no `zlib` CLI entry?** Our `zlib` algorithm emits a raw RFC-1950
> zlib stream. No standard CLI reads that container (`gzip` is RFC-1952,
> a different wrapper), so zlib is covered only by the stdlib `zlib`
> channel above, which is fully independent of our bundled copy.

## Running locally

```sh
# Python-library channel (from the python binding dir)
cd bindings/python
pip install -e '.[test]'        # or: pip install pytest zstandard brotli lz4
PYTHONPATH=. python -m pytest tests/test_interop.py -v

# CLI channel (from the repo root, against a built binding)
PYTHONPATH=bindings/python python tests/interop/cli_crosscheck.py
```

Both are also wired into CTest (`test_interop_py`, `test_interop_cli`) so
a normal `ctest` run after a CMake build executes them:

```sh
ctest --test-dir build -R interop --output-on-failure
```

## CI

- **`pr_build_and_test.yml`** installs the canonical Python libs and the
  codec CLIs on Linux/macOS/Windows, then both channels run as part of
  the existing `ctest` step on all three OSes.
- **`build_and_test_python.yml`** (cibuildwheel) installs the canonical
  libs via `test-requires`, so the Python-library channel runs on every
  wheel built — every OS, every Python version, including aarch64.

## Deferred: direct C/C++ codec interop

`TODO.md` also sketches C and C++ interop tests that call the bundled
codec libraries' C APIs directly. That is deliberately **not** done here:
calling the *same* static archive we link into the library is the least
independent reference available — it can't catch a framing divergence the
two interop channels above already cover against genuinely separate
implementations and binaries. If a future need arises (e.g. validating
against a codec version we don't bundle), the codec headers are installed
under `algorithms/dist/include/<algo>/` during a build and the IMPORTED
`<algo>_library` targets are linkable, so a `tests/test_interop.c` target
would be straightforward to add.
