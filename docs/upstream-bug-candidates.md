# Upstream bug candidates

Bugs in other people's libraries that the ecosystem benchmarks ran into. Nothing here has been reported yet; this file is for review before anyone files anything. Reproduced on 2026-10-25 on Node 24.12.0 (macOS, M-series) with the versions pinned in `benchmarks/drivers/wasm/package.json`.

Test data is the Silesia corpus, as fetched by `benchmarks/corpus/fetch.py` into `benchmarks/corpus/data/` (the reproductions below read from there). Both bugs were found because the benchmark harness round-trips every record and, for lz4js, also decodes the output with the reference implementation.

## fflate 0.8.3: streaming `Deflate` emits invalid output at level 3 and up on a large binary

**What happens.** Pushing the 7,251,944-byte Silesia `sao` file through `new Deflate({ level })` produces a raw DEFLATE stream that `node:zlib` rejects with `invalid distance too far back`. Level 1 is fine; levels 3, 4, 6 and 9 fail (level 2 was not tried). The one-shot `deflateSync` at level 6 on the same data is fine.

**Chunk size does not matter.** It fails the same way when the whole file is a single `push(data, true)`, when it is fed in 64 KiB pieces with `final` on the last piece, and when it is fed in 64 KiB pieces followed by an empty final push. So this is not a misuse of the `final` flag, and it is not about chunk boundaries; it looks like the streaming path mishandles something once the input exceeds some internal window or block size.

**Scope.** `sao` is the only file we found it on, out of the six silesia files in the "silesia-mini" tier, so it may depend on the data. The Gzip and Zlib streaming classes share the Deflate core, so they are probably affected too, but we only reproduced the raw `Deflate` class. Not checked: whether older fflate versions have it.

**Repro.**

```js
import fs from "node:fs";
import zlib from "node:zlib";
import { Deflate, deflateSync } from "fflate";

const d = new Uint8Array(fs.readFileSync("sao")); // silesia/sao, 7,251,944 bytes
for (const level of [1, 3, 4, 6, 9]) {
  const parts = [];
  const s = new Deflate({ level });
  s.ondata = (c) => parts.push(c);
  s.push(d, true);
  try {
    zlib.inflateRawSync(Buffer.concat(parts));
    console.log(level, "ok");
  } catch (e) {
    console.log(level, e.message); // 3, 4, 6, 9: invalid distance too far back
  }
}
zlib.inflateRawSync(deflateSync(d, { level: 6 })); // fine
```

**Effect on our numbers.** The streaming-compress records for fflate are missing for `sao` at levels 4 and up (the levels the benchmark ran), so fflate's streaming averages over silesia cover five files, not six. The doc says the results are left out, but the tables do not mark it.

**A discrepancy to fix in `docs/ecosystem-benchmarks.md`.** It says "level 4 and up". The failure starts at level 3 (the benchmark only runs levels 1, 4, 6 and 10, so it never saw 3).

## lz4js 0.2.0: invalid LZ4 frames on some inputs

**What happens.** `lz4js.compress` returns a frame that lz4js itself decompresses back correctly, but that the reference implementation rejects. The `lz4` command line tool (Homebrew, `lz4 -t`) reports `Error 34 : Decompression error : ERROR_decompressionFailed`, and python-lz4 (`lz4.frame.decompress`) reports `LZ4F_decompress failed with code: ERROR_decompressionFailed`. compress-utils' decoder (liblz4) rejects them too.

**Affected inputs.** Silesia `samba` (21,606,400 bytes), `dickens` (10,192,446) and `x-ray` (8,474,240). `sao` produces a valid frame, so does the rest of the corpus we ran. All three failing files are large; we have not checked whether the threshold is size, block count or data. Unchecked: whether it is the frame header, the block data or the frame end that is wrong.

**Repro.**

```js
const l = require("lz4js");
const c = l.compress(new Uint8Array(fs.readFileSync("samba")));
fs.writeFileSync("samba.lz4", c);
// l.decompress(c) round-trips; `lz4 -t samba.lz4` fails with error 34
```

**Effect on our numbers.** lz4js's compress throughput and ratio on those three files describe output that other decoders cannot read. The lz4js rows in the doc are flagged with this, but a user comparing speeds should know that part of its speed may come from skipped work.

## Before filing

- Re-run both repros against the latest release of each library first; versions here are the ones pinned for the benchmarks, not necessarily the newest.
- Check each project's issue tracker for an existing report.
- Shrink the inputs if possible; a 7 MB file is a poor test case for a maintainer. The fflate one in particular is worth bisecting by truncating `sao`.
