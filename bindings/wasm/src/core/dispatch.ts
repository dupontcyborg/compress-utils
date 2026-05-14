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

import {
    checkStatus,
    decodeCString,
    loadModule,
    type WasmExports,
} from "./loader.js";
import {
    Algorithm,
    CompressError,
    Status,
    type AlgorithmName,
    type CompressOptions,
    type DecompressOptions,
} from "./types.js";

const DEFAULT_LEVEL = 5;
const STREAM_DRAIN_CHUNK = 64 * 1024;
/* Safety cap on drain() iterations. A correct codec returns Ok within a
 * few iterations per write — anything past this many BUF_TOO_SMALL retries
 * means the C side is stuck in a state where it neither emits output nor
 * signals completion. Throw rather than spin forever. */
const DRAIN_MAX_ITERATIONS = 1 << 16;

/** Tracks allocations so a single try/finally can free them all. */
class Arena {
    private readonly exports: WasmExports;
    private readonly ptrs: number[] = [];

    constructor(exports: WasmExports) { this.exports = exports; }

    alloc(bytes: number, algoName: AlgorithmName): number {
        // Empty-input compress/decompress is a valid edge case (codecs
        // produce a non-empty header/footer for an empty payload). Bump
        // zero-byte requests to 1 so cu_alloc returns a real pointer
        // backing zero useful bytes — passing the resulting (ptr, 0) to
        // the C ABI is well-defined; passing (NULL, 0) is not.
        if (bytes === 0) bytes = 1;
        const ptr = this.exports.cu_alloc(bytes);
        if (ptr === 0) {
            throw new CompressError(
                Status.Oom, algoName,
                `cu_alloc(${bytes}) returned NULL`,
            );
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
                Status.UnsupportedAlgo, algorithmName,
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
                this.algorithm, inPtr, inLen, outPtr, outLenPtr, level,
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
                // raw LZ4, current xz size_hint impl). Fall through to a
                // streaming decode so the one-shot API still works.
                return this.decompressStreaming(input);
            }

            const outPtr = arena.alloc(outSize, this.algorithmName);
            const outLenPtr = arena.alloc(4, this.algorithmName);
            this.writeU32(outLenPtr, outSize);
            const status = this.exports.cu_decompress(
                this.algorithm, inPtr, inLen, outPtr, outLenPtr,
            );
            checkStatus(this.exports, status, this.algorithmName);
            return this.readBytes(outPtr, this.readU32(outLenPtr));
        } finally {
            arena.freeAll();
        }
    }

    private decompressStreaming(input: Uint8Array): Uint8Array {
        const stream = this.createDecompressStream();
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
        const status = this.exports.cu_decompress_size_hint(
            this.algorithm, inPtr, inLen, sizePtr,
        );
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
            const status = this.exports.cu_compress_stream_create(
                this.algorithm, level, streamPP,
            );
            checkStatus(this.exports, status, this.algorithmName);
            return new CompressStream(this, this.readU32(streamPP));
        } finally {
            arena.freeAll();
        }
    }

    createDecompressStream(): DecompressStream {
        const arena = new Arena(this.exports);
        try {
            const streamPP = arena.alloc(4, this.algorithmName);
            this.writeU32(streamPP, 0);
            const status = this.exports.cu_decompress_stream_create(
                this.algorithm, streamPP,
            );
            checkStatus(this.exports, status, this.algorithmName);
            return new DecompressStream(this, this.readU32(streamPP));
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
    /** @internal */ newArena(): Arena { return new Arena(this.exports); }
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

    constructor(
        dispatcher: Dispatcher, handle: number,
        destroyFn: (handle: number) => void,
    ) {
        this.dispatcher = dispatcher;
        this.handle = handle;
        this.cleanup = { destroy: destroyFn, handle };
        streamFinalizer.register(this, this.cleanup, this);
    }

    protected ensureLive(): void {
        if (this.handle === 0) {
            throw new CompressError(
                Status.StreamState, this.dispatcher.algorithmName,
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
    [Symbol.dispose](): void { this.destroy(); }
}

export class CompressStream extends StreamBase {
    constructor(dispatcher: Dispatcher, handle: number) {
        super(dispatcher, handle, (h) =>
            dispatcher.exports.cu_compress_stream_destroy(h));
    }

    write(chunk: Uint8Array): Uint8Array {
        this.ensureLive();
        if (this.finished) {
            throw new CompressError(
                Status.StreamFinished, this.dispatcher.algorithmName,
                "write after finish",
            );
        }
        return drain(this.dispatcher, chunk, (inPtr, inLen, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_compress_stream_write(
                this.handle, inPtr, inLen, outPtr, outLenPtr,
            ),
        );
    }

    finish(): Uint8Array {
        this.ensureLive();
        this.finished = true;
        return drain(this.dispatcher, null, (_in, _len, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_compress_stream_finish(
                this.handle, outPtr, outLenPtr,
            ),
        );
    }
}

export class DecompressStream extends StreamBase {
    constructor(dispatcher: Dispatcher, handle: number) {
        super(dispatcher, handle, (h) =>
            dispatcher.exports.cu_decompress_stream_destroy(h));
    }

    write(chunk: Uint8Array): Uint8Array {
        this.ensureLive();
        return drain(this.dispatcher, chunk, (inPtr, inLen, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_decompress_stream_write(
                this.handle, inPtr, inLen, outPtr, outLenPtr,
            ),
        );
    }

    finish(): Uint8Array {
        this.ensureLive();
        this.finished = true;
        return drain(this.dispatcher, null, (_in, _len, outPtr, outLenPtr) =>
            this.dispatcher.exports.cu_decompress_stream_finish(
                this.handle, outPtr, outLenPtr,
            ),
        );
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
    call: (
        inPtr: number, inLen: number,
        outPtr: number, outLenPtr: number,
    ) => number,
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
                    Status.StreamState, dispatcher.algorithmName,
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
    for (const c of chunks) { merged.set(c, off); off += c.byteLength; }
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
    compress(input: Uint8Array, opts?: CompressOptions): Promise<Uint8Array>;
    decompress(input: Uint8Array, opts?: DecompressOptions): Promise<Uint8Array>;
    createCompressStream(opts?: CompressOptions): Promise<CompressStream>;
    createDecompressStream(): Promise<DecompressStream>;
    /** TransformStream that compresses bytes as they flow through. */
    compressionStream(opts?: CompressOptions): TransformStream<Uint8Array, Uint8Array>;
    /** TransformStream that decompresses bytes as they flow through. */
    decompressionStream(): TransformStream<Uint8Array, Uint8Array>;
    /** Library version, e.g. "0.6.0". */
    version(): Promise<string>;
    /**
     * Cap on decompressed size (bytes) for the one-shot decompress path.
     * Defaults to 1 GiB. Set 0 to disable. Streaming decompress is not
     * affected — bound your own buffer there. Thread-local in C; in WASM
     * it's effectively global to this module instance.
     */
    setMaxDecompressedSize(bytes: number): Promise<void>;
}

export type WasmResolver =
    (url: URL) => Promise<BufferSource | Response | PromiseLike<Response>>;

export function createBindings(
    algorithm: Algorithm,
    name: AlgorithmName,
    wasmUrl: URL,
    resolveWasm: WasmResolver,
): AlgorithmBindings {
    let dispatcherPromise: Promise<Dispatcher> | null = null;
    const getDispatcher = (): Promise<Dispatcher> => {
        if (!dispatcherPromise) {
            dispatcherPromise = (async () => {
                const source = await resolveWasm(wasmUrl);
                const exports = await loadModule(source);
                return new Dispatcher(exports, algorithm, name);
            })();
        }
        return dispatcherPromise;
    };

    return {
        async compress(input, opts) {
            return (await getDispatcher()).compress(input, opts);
        },
        async decompress(input, opts) {
            return (await getDispatcher()).decompress(input, opts);
        },
        async createCompressStream(opts) {
            return (await getDispatcher()).createCompressStream(opts);
        },
        async createDecompressStream() {
            return (await getDispatcher()).createDecompressStream();
        },
        compressionStream(opts) {
            let stream: CompressStream | undefined;
            return new TransformStream<Uint8Array, Uint8Array>({
                async start() {
                    stream = (await getDispatcher()).createCompressStream(opts);
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
        decompressionStream() {
            let stream: DecompressStream | undefined;
            return new TransformStream<Uint8Array, Uint8Array>({
                async start() {
                    stream = (await getDispatcher()).createDecompressStream();
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

