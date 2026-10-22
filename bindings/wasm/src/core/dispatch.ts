/**
 * Generic JS→C ABI dispatcher.
 *
 * Per-algorithm modules (algorithms/<algo>/index.ts) call `createBindings`
 * with their algorithm enum + wasm URL; that factory returns the public
 * surface (compress, decompress, streams, version, etc.) bound to a
 * lazily-instantiated wasm module. The dispatcher itself doesn't ship in
 * any user bundle — only the slice of it the algorithm uses, after
 * tree-shaking.
 */

import { checkStatus, decodeCString, loadModule, type WasmExports } from "./loader.js";
import {
    type Algorithm,
    CompressError,
    Status,
    type AlgorithmName,
    type CompressOptions,
    type CompressionStreamOptions,
    type DecompressOptions,
    type DecompressStreamOptions,
} from "./types.js";

const DEFAULT_LEVEL = 5;
const STREAM_DRAIN_CHUNK = 64 * 1024;
/* Safety cap on drain() iterations. A correct codec returns Ok within a
 * few iterations per write — anything past this many BUF_TOO_SMALL retries
 * means the C side is stuck in a state where it neither emits output nor
 * signals completion. Throw rather than spin forever. */
const DRAIN_MAX_ITERATIONS = 1 << 16;
/* CU_DSTREAM_SINGLE (compress_utils.h). */
const DSTREAM_SINGLE = 0x1;

/** Tracks allocations so a single try/finally can free them all. */
class Arena {
    private readonly exports: WasmExports;
    private readonly ptrs: number[] = [];

    constructor(exports: WasmExports) {
        this.exports = exports;
    }

    alloc(bytes: number, algoName: AlgorithmName): number {
        // Empty-input compress/decompress is a valid edge case (codecs
        // produce a non-empty header/footer for an empty payload). Bump
        // zero-byte requests to 1 so cu_alloc returns a real pointer
        // backing zero useful bytes — passing the resulting (ptr, 0) to
        // the C ABI is well-defined; passing (NULL, 0) is not.
        if (bytes === 0) bytes = 1;
        const ptr = this.exports.cu_alloc(bytes);
        if (ptr === 0) {
            throw new CompressError(Status.Oom, algoName, `cu_alloc(${bytes}) returned NULL`);
        }
        this.ptrs.push(ptr);
        return ptr;
    }

    freeAll(): void {
        for (const p of this.ptrs) this.exports.cu_free(p);
        this.ptrs.length = 0;
    }
}

export class Dispatcher {
    readonly algorithm: Algorithm;
    readonly algorithmName: AlgorithmName;
    /** @internal */ readonly exports: WasmExports;

    constructor(exports: WasmExports, algorithm: Algorithm, algorithmName: AlgorithmName) {
        this.exports = exports;
        this.algorithm = algorithm;
        this.algorithmName = algorithmName;
        if (exports.cu_algorithm_available(algorithm) === 0) {
            throw new CompressError(
                Status.UnsupportedAlgo,
                algorithmName,
                `algorithm "${algorithmName}" not compiled into this .wasm module`,
            );
        }
    }

    version(): string {
        return decodeCString(this.exports.memory, this.exports.cu_version());
    }

    setMaxDecompressedSize(bytes: number): void {
        this.exports.cu_set_max_decompressed_size?.(bytes);
    }

    /* ---------- one-shot ---------- */

    compress(input: Uint8Array, opts: CompressOptions = {}): Uint8Array {
        const level = opts.level ?? DEFAULT_LEVEL;
        const inLen = input.byteLength;
        const bound = this.exports.cu_compress_bound(inLen, this.algorithm);

        const arena = new Arena(this.exports);
        try {
            const inPtr = arena.alloc(inLen, this.algorithmName);
            const outPtr = arena.alloc(bound, this.algorithmName);
            const outLenPtr = arena.alloc(4, this.algorithmName);

            this.writeBytes(inPtr, input);
            this.writeU32(outLenPtr, bound);
            const status = this.exports.cu_compress(
                this.algorithm,
                inPtr,
                inLen,
                outPtr,
                outLenPtr,
                level,
            );
            checkStatus(this.exports, status, this.algorithmName);
            return this.readBytes(outPtr, this.readU32(outLenPtr));
        } finally {
            arena.freeAll();
        }
    }

    decompress(input: Uint8Array, opts: DecompressOptions = {}): Uint8Array {
        const inLen = input.byteLength;
        const arena = new Arena(this.exports);
        try {
            const inPtr = arena.alloc(inLen, this.algorithmName);
            this.writeBytes(inPtr, input);

            const outSize = opts.expectedSize ?? this.tryProbeSize(arena, inPtr, inLen);
            if (outSize === undefined) {
                // Wire format doesn't carry size (bz2, brotli, raw deflate,
                // current xz size_hint impl). Fall through to a
                // streaming decode so the one-shot API still works.
                return this.decompressStreaming(input);
            }

            const outPtr = arena.alloc(outSize, this.algorithmName);
            const outLenPtr = arena.alloc(4, this.algorithmName);
            this.writeU32(outLenPtr, outSize);
            const status = this.exports.cu_decompress(
                this.algorithm,
                inPtr,
                inLen,
                outPtr,
                outLenPtr,
            );
            checkStatus(this.exports, status, this.algorithmName);
            return this.readBytes(outPtr, this.readU32(outLenPtr));
        } finally {
            arena.freeAll();
        }
    }

    /* Bounded by the same cap as the one-shot path. */
    private decompressStreaming(input: Uint8Array): Uint8Array {
        const stream = this.createDecompressStream({
            maxOutput: this.exports.cu_get_max_decompressed_size(),
        });
        try {
            const body = stream.write(input);
            const tail = stream.finish();
            if (tail.byteLength === 0) return body;
            const out = new Uint8Array(body.byteLength + tail.byteLength);
            out.set(body, 0);
            out.set(tail, body.byteLength);
            return out;
        } finally {
            stream.destroy();
        }
    }

    private tryProbeSize(arena: Arena, inPtr: number, inLen: number): number | undefined {
        const sizePtr = arena.alloc(4, this.algorithmName);
        this.writeU32(sizePtr, 0);
        const status = this.exports.cu_decompress_size_hint(this.algorithm, inPtr, inLen, sizePtr);
        if (status === Status.Ok) return this.readU32(sizePtr);
        if (status === Status.SizeUnknown) return undefined;
        checkStatus(this.exports, status, this.algorithmName);
        return undefined;
    }

    /* ---------- streaming ---------- */

    createCompressStream(opts: CompressOptions = {}): CompressStream {
        const level = opts.level ?? DEFAULT_LEVEL;
        const arena = new Arena(this.exports);
        try {
            const streamPP = arena.alloc(4, this.algorithmName);
            this.writeU32(streamPP, 0);
            const status = this.exports.cu_compress_stream_create(this.algorithm, level, streamPP);
            checkStatus(this.exports, status, this.algorithmName);
            return new CompressStream(this, this.readU32(streamPP));
        } finally {
            arena.freeAll();
        }
    }

    createDecompressStream(opts: DecompressStreamOptions = {}): DecompressStream {
        const single = opts.singleStream === true;
        const arena = new Arena(this.exports);
        try {
            const streamPP = arena.alloc(4, this.algorithmName);
            this.writeU32(streamPP, 0);
            const status = this.exports.cu_decompress_stream_create_ex(
                this.algorithm,
                single ? DSTREAM_SINGLE : 0,
                streamPP,
            );
            checkStatus(this.exports, status, this.algorithmName);
            const handle = this.readU32(streamPP);
            if (opts.maxOutput) {
                // size_t is 32-bit here, and no output can exceed the 4 GiB
                // address space, so larger limits saturate.
                this.exports.cu_decompress_stream_set_max_output(
                    handle,
                    Math.min(opts.maxOutput, 0xffffffff),
                );
            }
            return new DecompressStream(this, handle, single);
        } finally {
            arena.freeAll();
        }
    }

    /* ---------- internal helpers (used by stream classes) ---------- */

    /** @internal */ writeBytes(ptr: number, src: Uint8Array): void {
        new Uint8Array(this.exports.memory.buffer, ptr, src.byteLength).set(src);
    }
    /** @internal */ readBytes(ptr: number, len: number): Uint8Array {
        return new Uint8Array(new Uint8Array(this.exports.memory.buffer, ptr, len));
    }
    /** @internal */ writeU32(ptr: number, value: number): void {
        new DataView(this.exports.memory.buffer).setUint32(ptr, value, true);
    }
    /** @internal */ readU32(ptr: number): number {
        return new DataView(this.exports.memory.buffer).getUint32(ptr, true);
    }
    /** @internal */ newArena(): Arena {
        return new Arena(this.exports);
    }
}

/* ----------------------------------------------------------------------- *
 * Streams
 *
 * Both classes support three cleanup paths:
 *   1. Explicit:  `stream.destroy()`
 *   2. Scoped:    `using stream = await createCompressStream()` (TS 5.2+ / Node 22+)
 *   3. Best-effort GC backstop: a FinalizationRegistry frees the C-side
 *      handle if the JS object is collected without either of the above.
 *      The registry's callback may run arbitrarily late — never rely on
 *      it; it exists so a leak doesn't accumulate forever.
 *
 * Approximate disposal semantics: `using` is strictly preferred. `destroy`
 * is unregistered from the finalizer the moment it runs, so double-destroy
 * is safe and the finalizer never double-frees.
 * ----------------------------------------------------------------------- */

interface StreamCleanup {
    destroy: (handle: number) => void;
    handle: number;
}

const streamFinalizer = new FinalizationRegistry<StreamCleanup>((c) => {
    if (c.handle !== 0) c.destroy(c.handle);
});

abstract class StreamBase {
    protected readonly dispatcher: Dispatcher;
    protected handle: number;
    protected finished = false;
    private readonly cleanup: StreamCleanup;

    constructor(dispatcher: Dispatcher, handle: number, destroyFn: (handle: number) => void) {
        this.dispatcher = dispatcher;
        this.handle = handle;
        this.cleanup = { destroy: destroyFn, handle };
        streamFinalizer.register(this, this.cleanup, this);
    }

    protected ensureLive(): void {
        if (this.handle === 0) {
            throw new CompressError(
                Status.StreamState,
                this.dispatcher.algorithmName,
                "stream has been destroyed",
            );
        }
    }

    destroy(): void {
        if (this.handle !== 0) {
            this.cleanup.destroy(this.handle);
            this.handle = 0;
            this.cleanup.handle = 0;
            streamFinalizer.unregister(this);
        }
    }

    /** Symbol.dispose — enables `using stream = await create*()`. */
    [Symbol.dispose](): void {
        this.destroy();
    }
}

export class CompressStream extends StreamBase {
    constructor(dispatcher: Dispatcher, handle: number) {
        super(dispatcher, handle, (h) => dispatcher.exports.cu_compress_stream_destroy(h));
    }

    write(chunk: Uint8Array): Uint8Array {
        this.ensureLive();
        if (this.finished) {
            throw new CompressError(
                Status.StreamFinished,
                this.dispatcher.algorithmName,
                "write after finish",
            );
        }
        return drain(this.dispatcher, chunk, (inPtr, inLen, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_compress_stream_write(
                this.handle,
                inPtr,
                inLen,
                outPtr,
                outLenPtr,
            ),
        );
    }

    /**
     * Nonterminal flush: returns the bytes a decoder needs to recover
     * everything written so far, and keeps the stream open for more writes.
     * Each flush costs a few bytes. Throws CompressError for snappy and bz2,
     * which can't flush mid-stream.
     */
    flush(): Uint8Array {
        this.ensureLive();
        if (this.finished) {
            throw new CompressError(
                Status.StreamFinished,
                this.dispatcher.algorithmName,
                "flush after finish",
            );
        }
        return drain(this.dispatcher, null, (_in, _len, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_compress_stream_flush(this.handle, outPtr, outLenPtr),
        );
    }

    finish(): Uint8Array {
        this.ensureLive();
        this.finished = true;
        return drain(this.dispatcher, null, (_in, _len, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_compress_stream_finish(this.handle, outPtr, outLenPtr),
        );
    }
}

export class DecompressStream extends StreamBase {
    private readonly single: boolean;
    private ended = false;
    private unused: Uint8Array = new Uint8Array(0);

    constructor(dispatcher: Dispatcher, handle: number, single = false) {
        super(dispatcher, handle, (h) => dispatcher.exports.cu_decompress_stream_destroy(h));
        this.single = single;
    }

    /**
     * Single-stream mode (`{ singleStream: true }`): true once the end of the
     * stream has been decoded and all its output returned. Always false
     * otherwise.
     */
    get eof(): boolean {
        return this.ended;
    }

    /**
     * Single-stream mode: the input bytes written after the end of the
     * stream, once `eof` is true; empty before that, and in the default mode.
     */
    get unusedData(): Uint8Array {
        // A copy, so a caller changing it can't change later reads.
        return this.unused.slice();
    }

    write(chunk: Uint8Array): Uint8Array {
        this.ensureLive();
        const out = drain(this.dispatcher, chunk, (inPtr, inLen, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_decompress_stream_write(
                this.handle,
                inPtr,
                inLen,
                outPtr,
                outLenPtr,
            ),
        );
        // Only a call that succeeds can reach the end.
        this.checkEnd();
        return out;
    }

    finish(): Uint8Array {
        this.ensureLive();
        this.finished = true;
        const out = drain(this.dispatcher, null, (_in, _len, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_decompress_stream_finish(this.handle, outPtr, outLenPtr),
        );
        this.checkEnd();
        return out;
    }

    /* Copies the unused input out once the end is reached: the C pointer is
     * only valid until the next call on the stream. */
    private checkEnd(): void {
        if (!this.single || this.ended || this.handle === 0) return;
        const exports = this.dispatcher.exports;
        if (exports.cu_decompress_stream_eof(this.handle) === 0) return;
        const arena = this.dispatcher.newArena();
        try {
            const dataPP = arena.alloc(4, this.dispatcher.algorithmName);
            const lenP = arena.alloc(4, this.dispatcher.algorithmName);
            const status = exports.cu_decompress_stream_unused(this.handle, dataPP, lenP);
            checkStatus(exports, status, this.dispatcher.algorithmName);
            const len = this.dispatcher.readU32(lenP);
            this.unused =
                len > 0
                    ? this.dispatcher.readBytes(this.dispatcher.readU32(dataPP), len)
                    : new Uint8Array(0);
            this.ended = true;
        } finally {
            arena.freeAll();
        }
    }
}

/**
 * Drain protocol: repeat the call until it returns Ok with no overflow.
 * BUF_TOO_SMALL means the stream has more output buffered; the C side
 * remembers any unconsumed input, so on retry we pass (NULL, 0).
 */
function drain(
    dispatcher: Dispatcher,
    input: Uint8Array | null,
    call: (inPtr: number, inLen: number, outPtr: number, outLenPtr: number) => number,
): Uint8Array {
    const arena = dispatcher.newArena();
    const chunks: Uint8Array[] = [];
    let total = 0;

    try {
        const inLen = input?.byteLength ?? 0;
        const inPtr0 = inLen > 0 ? arena.alloc(inLen, dispatcher.algorithmName) : 0;
        if (inPtr0 !== 0 && input) dispatcher.writeBytes(inPtr0, input);

        const outPtr = arena.alloc(STREAM_DRAIN_CHUNK, dispatcher.algorithmName);
        const outLenPtr = arena.alloc(4, dispatcher.algorithmName);

        let inPtr = inPtr0;
        let remainingIn = inLen;
        for (let i = 0; ; i++) {
            if (i >= DRAIN_MAX_ITERATIONS) {
                throw new CompressError(
                    Status.StreamState,
                    dispatcher.algorithmName,
                    `drain exceeded ${DRAIN_MAX_ITERATIONS} iterations without completion`,
                );
            }
            dispatcher.writeU32(outLenPtr, STREAM_DRAIN_CHUNK);
            const status = call(inPtr, remainingIn, outPtr, outLenPtr);
            const produced = dispatcher.readU32(outLenPtr);
            if (produced > 0) {
                chunks.push(dispatcher.readBytes(outPtr, produced));
                total += produced;
            }
            if (status === Status.Ok) break;
            if (status !== Status.BufTooSmall) {
                checkStatus(dispatcher.exports, status, dispatcher.algorithmName);
            }
            inPtr = 0;
            remainingIn = 0;
        }
    } finally {
        arena.freeAll();
    }

    if (chunks.length === 1) return chunks[0]!;
    const merged = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
        merged.set(c, off);
        off += c.byteLength;
    }
    return merged;
}

/* ----------------------------------------------------------------------- *
 * Public bindings factory.
 *
 * Each algorithm subpath imports this, calls it once with its enum value,
 * canonical name, and .wasm URL, and re-exports the result. Lazy: the
 * module is instantiated on the first call, then cached.
 * ----------------------------------------------------------------------- */

export interface AlgorithmBindings {
    /** Initializes this subpath's WASM module for subsequent synchronous calls. */
    preload(): Promise<void>;
    /** Compresses synchronously after preload() or an asynchronous call completes. */
    compressSync(input: Uint8Array, opts?: CompressOptions): Uint8Array;
    /** Decompresses synchronously after preload() or an asynchronous call completes. */
    decompressSync(input: Uint8Array, opts?: DecompressOptions): Uint8Array;
    /** Creates an encoder synchronously after initialization. */
    createCompressStreamSync(opts?: CompressOptions): CompressStream;
    /**
     * Creates a decoder synchronously after initialization. With
     * `singleStream`, it stops at the end of the first frame; see
     * `DecompressStream.eof` / `unusedData`.
     */
    createDecompressStreamSync(opts?: DecompressStreamOptions): DecompressStream;
    compress(input: Uint8Array, opts?: CompressOptions): Promise<Uint8Array>;
    decompress(input: Uint8Array, opts?: DecompressOptions): Promise<Uint8Array>;
    createCompressStream(opts?: CompressOptions): Promise<CompressStream>;
    /**
     * Creates a decoder. With `singleStream`, it stops at the end of the
     * first frame; see `DecompressStream.eof` / `unusedData`.
     */
    createDecompressStream(opts?: DecompressStreamOptions): Promise<DecompressStream>;
    /**
     * TransformStream that compresses bytes as they flow through. With
     * `flushEachChunk`, each output chunk is decodable on arrival.
     */
    compressionStream(opts?: CompressionStreamOptions): TransformStream<Uint8Array, Uint8Array>;
    /**
     * TransformStream that decompresses bytes as they flow through. It always
     * decodes concatenated frames as one stream: a TransformStream can't hand
     * back bytes after the end, so use `createDecompressStream({ singleStream:
     * true })` for that. `maxOutput` limits the total decompressed output; the
     * stream errors with a `CompressError` (code `SizeLimit`) past it.
     */
    decompressionStream(
        opts?: Pick<DecompressStreamOptions, "maxOutput">,
    ): TransformStream<Uint8Array, Uint8Array>;
    /** Library version, e.g. "0.6.0". */
    version(): Promise<string>;
    /**
     * Cap on decompressed size (bytes) for one-shot decompress, for every
     * algorithm. Defaults to 1 GiB. Set 0 to disable. Streams have no limit
     * unless given `maxOutput` (raw LZ4/LZO and Snappy also capture this cap
     * at creation). Global to this module instance.
     */
    setMaxDecompressedSize(bytes: number): Promise<void>;
}

export type WasmResolver = (url: URL) => Promise<BufferSource | Response | PromiseLike<Response>>;

/** Shared WASM initialization for wire variants using the same codec asset. */
const wasmModulePromises = new WeakMap<WasmResolver, Map<string, Promise<WasmExports>>>();

/** Loads one module per asset/resolver; failed loads remain retryable. */
function loadSharedModule(wasmUrl: URL, resolveWasm: WasmResolver): Promise<WasmExports> {
    let modules = wasmModulePromises.get(resolveWasm);
    if (!modules) {
        modules = new Map();
        wasmModulePromises.set(resolveWasm, modules);
    }
    const cached = modules.get(wasmUrl.href);
    if (cached) return cached;
    const currentModules = modules;
    const pending = (async () => loadModule(await resolveWasm(wasmUrl)))().catch(
        (error: unknown) => {
            currentModules.delete(wasmUrl.href);
            throw error;
        },
    );
    modules.set(wasmUrl.href, pending);
    return pending;
}

export function createBindings(
    algorithm: Algorithm,
    name: AlgorithmName,
    wasmUrl: URL,
    resolveWasm: WasmResolver,
): AlgorithmBindings {
    let dispatcherPromise: Promise<Dispatcher> | null = null;
    let dispatcher: Dispatcher | null = null;
    const getDispatcher = (): Promise<Dispatcher> => {
        if (!dispatcherPromise) {
            dispatcherPromise = (async () => {
                const exports = await loadSharedModule(wasmUrl, resolveWasm);
                dispatcher = new Dispatcher(exports, algorithm, name);
                return dispatcher;
            })().catch((error: unknown) => {
                dispatcherPromise = null;
                throw error;
            });
        }
        return dispatcherPromise;
    };

    /** Requires completed initialization without starting asynchronous work. */
    const getInitializedDispatcher = (): Dispatcher => {
        if (!dispatcher) {
            throw new Error(`${name}: call and await preload() before using synchronous methods`);
        }
        return dispatcher;
    };

    return {
        async preload() {
            await getDispatcher();
        },
        compressSync(input, opts) {
            return getInitializedDispatcher().compress(input, opts);
        },
        decompressSync(input, opts) {
            return getInitializedDispatcher().decompress(input, opts);
        },
        createCompressStreamSync(opts) {
            return getInitializedDispatcher().createCompressStream(opts);
        },
        createDecompressStreamSync(opts) {
            return getInitializedDispatcher().createDecompressStream(opts);
        },
        async compress(input, opts) {
            return (await getDispatcher()).compress(input, opts);
        },
        async decompress(input, opts) {
            return (await getDispatcher()).decompress(input, opts);
        },
        async createCompressStream(opts) {
            return (await getDispatcher()).createCompressStream(opts);
        },
        async createDecompressStream(opts) {
            return (await getDispatcher()).createDecompressStream(opts);
        },
        compressionStream(opts) {
            let stream: CompressStream | undefined;
            return new TransformStream<Uint8Array, Uint8Array>({
                async start() {
                    stream = (await getDispatcher()).createCompressStream(opts);
                },
                transform(chunk, controller) {
                    let out = stream!.write(chunk);
                    if (opts?.flushEachChunk) {
                        // One output chunk per input chunk, decodable on its own.
                        const flushed = stream!.flush();
                        const joined = new Uint8Array(out.byteLength + flushed.byteLength);
                        joined.set(out, 0);
                        joined.set(flushed, out.byteLength);
                        out = joined;
                    }
                    if (out.byteLength > 0) controller.enqueue(out);
                },
                flush(controller) {
                    try {
                        const tail = stream!.finish();
                        if (tail.byteLength > 0) controller.enqueue(tail);
                    } finally {
                        stream!.destroy();
                    }
                },
            });
        },
        decompressionStream(opts) {
            let stream: DecompressStream | undefined;
            return new TransformStream<Uint8Array, Uint8Array>({
                async start() {
                    stream = (await getDispatcher()).createDecompressStream(opts);
                },
                transform(chunk, controller) {
                    const out = stream!.write(chunk);
                    if (out.byteLength > 0) controller.enqueue(out);
                },
                flush(controller) {
                    try {
                        const tail = stream!.finish();
                        if (tail.byteLength > 0) controller.enqueue(tail);
                    } finally {
                        stream!.destroy();
                    }
                },
            });
        },
        async version() {
            return (await getDispatcher()).version();
        },
        async setMaxDecompressedSize(bytes) {
            (await getDispatcher()).setMaxDecompressedSize(bytes);
        },
    };
}
