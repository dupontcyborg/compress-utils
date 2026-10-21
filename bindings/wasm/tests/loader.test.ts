/**
 * The WASM loader and its WASI reactor shim, plus dispatcher behaviour that
 * the per-codec suites can't reach: the streaming-instantiate path a
 * browser resolver takes (a fetch Response), and a module that lacks the
 * requested algorithm.
 */

import { readFile } from "node:fs/promises";
import { describe, expect, it } from "vitest";
import { createBindings } from "../src/core/dispatch.js";
import { loadModule } from "../src/core/loader.js";
import { Algorithm, CompressError } from "../src/core/types.js";

/* WASI errno values the shim returns (wasi_snapshot_preview1). */
const ERRNO_BADF = 8;
const ERRNO_NOSYS = 52;
/* CompressError status: algorithm not compiled into the module. */
const UNSUPPORTED_ALGO = 5;

const zlibWasm = new URL("../dist/algorithms/zlib/zlib.wasm", import.meta.url);
const input = new TextEncoder().encode("loader payload\n".repeat(200));

/** WASI imports the shim provides, with the result each stub returns. */
const NOSYS = [
    "fd_write",
    "fd_close",
    "fd_seek",
    "fd_read",
    "fd_fdstat_get",
    "fd_fdstat_set_flags",
    "fd_filestat_get",
    "path_open",
    "poll_oneoff",
    "clock_time_get",
];
const BADF = ["fd_prestat_get", "fd_prestat_dir_name"];
const ZERO = ["sched_yield", "environ_sizes_get", "environ_get"];

type Shim = Record<string, (...args: number[]) => number | undefined> & {
    memory: WebAssembly.Memory;
};

describe("WASI reactor shim", () => {
    it("re-exported WASI imports return the documented stub results", async () => {
        const shim = (await loadModule(wasiProbeModule())) as unknown as Shim;
        for (const name of NOSYS) expect(shim[name]!(), name).toBe(ERRNO_NOSYS);
        for (const name of BADF) expect(shim[name]!(), name).toBe(ERRNO_BADF);
        for (const name of ZERO) expect(shim[name]!(), name).toBe(0);
    });

    it("proc_exit throws with the exit code instead of exiting silently", async () => {
        const shim = (await loadModule(wasiProbeModule())) as unknown as Shim;
        expect(() => shim.proc_exit!(3)).toThrow("wasm proc_exit(3)");
    });

    it("random_get fills the requested range, in chunks past the WebCrypto cap", async () => {
        const shim = (await loadModule(wasiProbeModule())) as unknown as Shim;
        const offset = 16;
        const len = 65536 + 4096; // two getRandomValues calls
        const view = new Uint8Array(shim.memory.buffer);
        view.fill(0);
        expect(shim.random_get!(offset, len)).toBe(0);
        // Bytes outside the range are untouched.
        expect(view.subarray(0, offset).every((b) => b === 0)).toBe(true);
        expect(view[offset + len]).toBe(0);
        // Both chunks received entropy (all-zero 4 KiB of CSPRNG output is impossible).
        expect(view.subarray(offset, offset + 4096).some((b) => b !== 0)).toBe(true);
        expect(view.subarray(offset + 65536, offset + len).some((b) => b !== 0)).toBe(true);
    });
});

describe("loadModule sources", () => {
    it("instantiates from a fetch Response (streaming compile path)", async () => {
        const toResponse = async (url: URL) =>
            new Response(await readFile(url), { headers: { "Content-Type": "application/wasm" } });
        const bindings = createBindings(Algorithm.Zlib, "zlib", zlibWasm, toResponse);
        const compressed = await bindings.compress(input);
        expect(await bindings.decompress(compressed)).toEqual(input);
        expect(await bindings.version()).toMatch(/^\d+\.\d+\.\d+$/);
    });

    it("instantiates from a typed-array view as well as a buffer", async () => {
        const bytes = await readFile(zlibWasm);
        const exports = await loadModule(new Uint8Array(bytes));
        expect(exports.cu_algorithm_available(Algorithm.Zlib)).toBe(1);
    });
});

describe("dispatcher", () => {
    it("rejects an algorithm the module was not built with as UnsupportedAlgo", async () => {
        // zlib.wasm carries zlib/gzip/deflate, not zstd.
        const bindings = createBindings(Algorithm.Zstd, "zstd", zlibWasm, readFile);
        const err = await bindings.preload().catch((e: unknown) => e);
        expect(err).toBeInstanceOf(CompressError);
        expect(err).toMatchObject({ code: UNSUPPORTED_ALGO, algorithm: "zstd" });
        // Failure leaves the bindings uninitialized, so sync calls still refuse.
        expect(() => bindings.compressSync(input)).toThrow(/preload/);
        await expect(bindings.compress(input)).rejects.toMatchObject({ code: UNSUPPORTED_ALGO });
    });
});

/**
 * A minimal module that imports every WASI function the shim provides and
 * re-exports each one, plus a 2-page exported memory, so the stubs can be
 * called directly through `loadModule`'s real import object.
 */
function wasiProbeModule(): Uint8Array {
    const T_VOID_I32 = 0; // () -> i32
    const T_I32_VOID = 1; // (i32) -> ()
    const T_I32I32_I32 = 2; // (i32, i32) -> i32
    const I32 = 0x7f;
    const types = [
        [0x60, 0, 1, I32],
        [0x60, 1, I32, 0],
        [0x60, 2, I32, I32, 1, I32],
    ];
    const imports: [string, number][] = [
        ["proc_exit", T_I32_VOID],
        ["random_get", T_I32I32_I32],
        ...[...NOSYS, ...BADF, ...ZERO].map((n): [string, number] => [n, T_VOID_I32]),
    ];
    const name = (s: string) => [s.length, ...new TextEncoder().encode(s)];
    const vec = (items: number[][]) => [...uleb(items.length), ...items.flat()];
    const section = (id: number, body: number[]) => [id, ...uleb(body.length), ...body];

    return new Uint8Array([
        ...[0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00],
        ...section(1, vec(types)),
        ...section(
            2,
            vec(imports.map(([n, t]) => [...name("wasi_snapshot_preview1"), ...name(n), 0x00, t])),
        ),
        ...section(5, vec([[0x00, 2]])),
        ...section(
            7,
            vec([
                [...name("memory"), 0x02, 0],
                ...imports.map(([n], i) => [...name(n), 0x00, ...uleb(i)]),
            ]),
        ),
    ]);
}

function uleb(value: number): number[] {
    const out: number[] = [];
    do {
        let byte = value & 0x7f;
        value >>>= 7;
        if (value !== 0) byte |= 0x80;
        out.push(byte);
    } while (value !== 0);
    return out;
}
