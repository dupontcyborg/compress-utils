# README skeleton

Every binding README follows the same section order, so a reader who learns one can find things in all of them. `tools/check-readmes.py` enforces the parts that can be checked mechanically and runs on every PR (see `.github/workflows/readme-check.yml`).

## What is generated and what is hand-written

The algorithm table in every README, and the language table in the root README, are generated from [`docs/readme/manifest.json`](readme/manifest.json) into marked regions:

```markdown
<!-- BEGIN:algorithms -->
...
<!-- END:algorithms -->
```

To change a wire-format description, add an algorithm, or reorder rows, edit the manifest and run `tools/check-readmes.py --fix`. Never edit inside a region by hand; the checker fails the PR if the region doesn't match the manifest.

The manifest's algorithm list is itself checked against the `cu_algorithm_t` enum in `include/compress_utils.h`, so adding an algorithm to the C ABI without documenting it fails CI. When you follow [`adding-an-algorithm.md`](adding-an-algorithm.md), add the manifest entry as part of it.

## Binding README sections

Required, in this order. Extra sections may go anywhere between them. Headings are matched case-insensitively at any level from `##` down, and the heading text only has to match the pattern shown.

| # | Section | Heading must match | Contents |
|---|---------|--------------------|----------|
| 1 | Title, badges, pitch | (H1) | One to two benefit-led sentences a package-registry visitor understands (one API for every algorithm, what ships, what it is backed by); the Python README is the model. No numeric performance claims in the pitch, the chart carries them. Markdown badges only, in this order: a live version badge (the registry's `img.shields.io/{pypi,npm,crates}/v/...` where the package is published, otherwise `img.shields.io/github/v/{release,tag}/...`), other registry badges (downloads, language versions), a hosted API docs badge where the registry generates one (pkg.go.dev, docs.rs), the CI status badge for the binding's workflow, then the license badge. |
| 2 | Installation | `Install…` | The one command, then what the build needs (toolchain, wheel contents, runtimes). |
| 3 | Quick start | `Quick start` | A complete one-shot compress and decompress that runs as pasted. |
| 4 | Performance | `Performance` | See below. The generated chart block, directly after Quick start. |
| 5 | Supported algorithms | `Algorithms` or `Supported algorithms` | The generated table. Prose underneath may cover availability checks and aliases. |
| 6 | Raw blocks and output capacity | `Raw blocks…` | The capacity argument for `deflate`, `lz4_raw` and `lzo`, with one example. Keep the paragraph identical in substance across bindings. |
| 7 | Streaming | `…streaming…` | The idiomatic streaming API for the language, including how it is flushed or closed. |
| 8 | Compression levels | `…levels` | `1` fastest to `10` smallest, default `5`, mapped to each codec's native range. |
| 9 | Errors and limits | `Error…` | The error type and how to read the status code, `version()`, `set_max_decompressed_size` and its default. |
| 10 | Concurrency | `…concurrency…` or `…thread…` | What is safe to share between threads, goroutines or workers, and what the binding does about locks. A short paragraph is fine, but it must exist: the bindings differ, and readers compare them. |
| 11 | License | `License` | `MIT. See LICENSE.`, then a `---` rule and the footer line. |

**Platform support** (runtimes, operating systems) is optional and goes between Concurrency and Performance when present.

### Performance

Required, short and specific, and generated: the block between `<!-- bench:start -->` and `<!-- bench:end -->` written by `benchmarks/publish.py` holds the chart (a `<picture>` with light and dark images at absolute URLs), a one-sentence caption, a measured-on line, and links to the benchmark pages. Do not hand-edit it, and do not compare against the C core in binding READMEs (that lives in `docs/benchmarks/bindings.md`). Hand-written text goes after the block and only says what is true of this binding (GIL release, cgo call cost, bundle size); keep numbers out of it, they go stale. The chart is generated and the pages carry the full tables, so do not add a table or repeat the cross-language chart here.

## Root README sections

Required, in this order: **Supported languages** (generated), **Supported algorithms** (generated), **Benchmarks**, **Building from source**, **License**. The title, badges, pitch and diagram come first. Acknowledgments goes before License. Other sections may sit between them.

## Rules that apply to every README

- **Same header and footer.** Badges are markdown, not HTML, and always include a live version badge, a CI status badge and a license badge. Version badges are resolved by shields.io, never a hardcoded number. Every README ends with a `---` rule and a line ending `Built by [Nico Dupont](https://nico.codes).`, and binding READMEs start that line with `Part of [compress-utils](https://github.com/dupontcyborg/compress-utils).`
- **No table of contents.** GitHub, PyPI and npm render their own outline, and the READMEs are short enough to scan.
- **One paragraph or list item per line.** No hard wrapping; editors and email clients wrap for you.
- **Version examples match the source of truth.** `version()` examples carry the `DEFAULT_VERSION` from `CMakeLists.txt`, or no literal at all. `tools/sync-versions.py` rewrites them on a version bump, and `version-check.yml` keeps that value aligned with the git tag.
- **Counts match the manifest.** "Eight algorithms" is the number of `core` entries, "seven upstream libraries" is the number of entries in `codec-versions.json`, and the `algorithms-N` and `languages-N` badges follow the manifest.
- **Links resolve.** Relative links must exist, and `#anchors` must match a heading. The Python and WASM READMEs render on PyPI and npm, which don't resolve repo-relative links, so they use absolute `https://github.com/dupontcyborg/compress-utils/blob/main/...` URLs.
- **No rustdoc-hidden lines.** The Rust README is not compiled as rustdoc, so `# Ok::<...>` lines would show up literally. Use `?` inside a function body in the example or drop the line.
- **Metadata agrees.** `license` is `MIT` and `repository` is the same URL in `Cargo.toml`, `pyproject.toml` and `package.json`.

## Running the check

```sh
python3 tools/check-readmes.py          # report problems, exit 1 if any
python3 tools/check-readmes.py --fix    # regenerate the marked regions, then report what's left
```

It needs Python 3.11 or newer and has no dependencies.

## Not enforced

Prose quality, example correctness and whether a Concurrency paragraph is accurate are still on the reviewer. If an example in a README stops compiling, that should be caught by the binding's own tests; consider testing the Quick start snippet directly if a binding's README keeps drifting.
