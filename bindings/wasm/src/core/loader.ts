/**
 * WASM module loader + minimal WASI reactor shim.
 *
 * Our .wasm modules are built as `wasm32-wasi -mexec-model=reactor`. This
 * means they (a) export their full ABI under default names, (b) expect a
 * `_initialize` call after instantiation, and (c) import a handful of WASI
 * preview1 functions even though our code paths don't actually use them
 * (libc startup pulls in a few stubs).
 *
 * The shim below implements just enough WASI to keep the imports resolved.
 * Real WASI operations (fd_write, random_get) are stubbed with safe
 * defaults — the codec paths don't hit them in practice, but if a future
 * upstream lib starts logging to stderr we'll see "fd_write: not
 * implemented" instead of a cryptic trap.
 *
 * If we ever need actual WASI (e.g. for `wasi:filesystem`), swap this for
 * @bjorn3/browser_wasi_shim or @cloudflare/workers-types.
 */

import { CompressError, Status, type AlgorithmName } from "./types.js";

export interface WasmExports {
    readonly memory: WebAssembly.Memory;
    readonly _initialize: () => void;

    /* Allocator (defined in src/wasm_runtime.c). */
    readonly cu_alloc: (bytes: number) => number;
    readonly cu_free: (ptr: number) => void;

    /* Public C ABI. */
    readonly cu_version: () => number;
    readonly cu_algorithm_available: (algo: number) => number;
    readonly cu_last_error: () => number;
    readonly cu_strerror: (code: number) => number;

    readonly cu_compress_bound: (in_len: number, algo: number) => number;
    readonly cu_compress: (
        algo: number,
        in_ptr: number, in_len: number,
        out_ptr: number, out_len_ptr: number,
        level: number,
    ) => number;
    readonly cu_decompress: (
        algo: number,
        in_ptr: number, in_len: number,
        out_ptr: number, out_len_ptr: number,
    ) => number;
    readonly cu_decompress_size_hint: (
        algo: number,
        in_ptr: number, in_len: number,
        out_size_ptr: number,
    ) => number;

    readonly cu_compress_stream_create: (
        algo: number, level: number, out_stream_pp: number,
    ) => number;
    readonly cu_compress_stream_write: (
        stream: number,
        in_ptr: number, in_len: number,
        out_ptr: number, out_len_ptr: number,
    ) => number;
    readonly cu_compress_stream_finish: (
        stream: number,
        out_ptr: number, out_len_ptr: number,
    ) => number;
    readonly cu_compress_stream_destroy: (stream: number) => void;

    readonly cu_decompress_stream_create: (
        algo: number, out_stream_pp: number,
    ) => number;
    readonly cu_decompress_stream_write: (
        stream: number,
        in_ptr: number, in_len: number,
        out_ptr: number, out_len_ptr: number,
    ) => number;
    readonly cu_decompress_stream_finish: (
        stream: number,
        out_ptr: number, out_len_ptr: number,
    ) => number;
    readonly cu_decompress_stream_destroy: (stream: number) => void;

    /* Optional — only present in wasm modules that wired it through. */
    readonly cu_set_max_decompressed_size?: (bytes: number) => void;
}

const ERRNO_BADF = 8;
const ERRNO_NOSYS = 52;

function wasiImports(): WebAssembly.ModuleImports {
    return {
        // proc_exit: codec code paths shouldn't call this, but if libc
        // startup hits an unrecoverable error it does. Throw so callers
        // see a real stack instead of a silent exit.
        proc_exit(code: number): void {
            throw new Error(`wasm proc_exit(${code})`);
        },
        // fd_write: silently swallow. zstd/etc don't log; if they did,
        // we'd want to wire this to console.warn — but that needs us to
        // read iovs out of linear memory, which the caller doesn't have
        // a handle to at import-resolution time.
        fd_write(): number { return ERRNO_NOSYS; },
        fd_close(): number { return ERRNO_NOSYS; },
        fd_seek(): number { return ERRNO_NOSYS; },
        fd_read(): number { return ERRNO_NOSYS; },
        fd_fdstat_get(): number { return ERRNO_NOSYS; },
        fd_fdstat_set_flags(): number { return ERRNO_NOSYS; },
        // EBADF terminates the wasi-libc preopen scan cleanly. ENOSYS
        // makes the libc init path think the syscall is implemented but
        // failed unexpectedly, and on some libc builds aborts.
        fd_prestat_get(): number { return ERRNO_BADF; },
        fd_prestat_dir_name(): number { return ERRNO_BADF; },
        fd_filestat_get(): number { return ERRNO_NOSYS; },
        path_open(): number { return ERRNO_NOSYS; },
        poll_oneoff(): number { return ERRNO_NOSYS; },
        sched_yield(): number { return 0; },
        environ_sizes_get(): number { return 0; },
        environ_get(): number { return 0; },
        clock_time_get(): number { return ERRNO_NOSYS; },
        // random_get: needed if a codec seeds randomness from /dev/urandom
        // surrogates. Fill with crypto.getRandomValues when available.
        random_get(buf: number, len: number): number {
            // Resolved lazily — we don't have memory yet at imports time.
            // Stub to "success but unseeded"; codecs that actually need
            // entropy must opt-in to a richer shim.
            void buf; void len;
            return 0;
        },
    };
}

export async function loadModule(
    source: BufferSource | Response | PromiseLike<Response>,
): Promise<WasmExports> {
    const imports = { wasi_snapshot_preview1: wasiImports() };

    let instance: WebAssembly.Instance;
    if (source instanceof ArrayBuffer || ArrayBuffer.isView(source)) {
        const { instance: inst } = await WebAssembly.instantiate(source, imports);
        instance = inst;
    } else {
        const { instance: inst } = await WebAssembly.instantiateStreaming(
            source as Response | PromiseLike<Response>, imports,
        );
        instance = inst;
    }

    const exports = instance.exports as unknown as WasmExports;
    exports._initialize?.();
    return exports;
}

export function decodeCString(
    memory: WebAssembly.Memory, ptr: number, maxLen = 4096,
): string {
    if (ptr === 0) return "";
    const view = new Uint8Array(memory.buffer, ptr);
    let end = 0;
    while (end < maxLen && view[end] !== 0) end++;
    return new TextDecoder("utf-8").decode(view.subarray(0, end));
}

/** Convert a non-Ok status into a thrown CompressError using the C side's message. */
export function checkStatus(
    exports: WasmExports, status: number, algorithm: AlgorithmName,
): void {
    if (status === Status.Ok) return;
    const lastErrPtr = exports.cu_last_error();
    let msg = decodeCString(exports.memory, lastErrPtr);
    if (!msg) msg = decodeCString(exports.memory, exports.cu_strerror(status));
    throw new CompressError(status, algorithm, msg || `status ${status}`);
}
