/** Buffered chunk-input adapters for codecs whose wire format is one raw block. */
import {
    CompressError,
    Status,
    type AlgorithmName,
    type CompressOptions,
    type DecompressOptions,
} from "./types.js";

/** Limits memory retained while collecting one block. */
export interface BlockStreamOptions {
    /** Maximum buffered input bytes. Defaults to 64 MiB; zero accepts only empty input. */
    maxInputSize?: number;
}
/** Options for a raw block encoder accepting fragmented input. */
export type BlockCompressOptions = CompressOptions & BlockStreamOptions;
/** Raw block decoding requires caller-provided output capacity. */
export type BlockDecompressOptions = DecompressOptions &
    BlockStreamOptions & { expectedSize: number };

/** Collects one block, emitting its result only from finish(). No container framing. */
export class BufferedBlockStream {
    private chunks: Uint8Array[] = [];
    private inputSize = 0;
    private finished = false;
    private destroyed = false;
    private readonly maxInputSize: number;

    /** Creates a bounded adapter around an initialized one-shot codec. */
    constructor(
        private readonly algorithm: AlgorithmName,
        private readonly processBlock: (input: Uint8Array) => Uint8Array,
        maxInputSize = 64 * 1024 * 1024,
    ) {
        if (!Number.isSafeInteger(maxInputSize) || maxInputSize < 0 || maxInputSize > 0x7fffffff) {
            throw new CompressError(
                Status.InvalidArg,
                algorithm,
                "maxInputSize must be an integer 0..2147483647",
            );
        }
        this.maxInputSize = maxInputSize;
    }

    /** Copies a fragment; returns no output until the complete block is finished. */
    write(chunk: Uint8Array): Uint8Array {
        this.ensureWritable();
        if (!(chunk instanceof Uint8Array)) {
            throw new CompressError(Status.InvalidArg, this.algorithm, "input must be Uint8Array");
        }
        if (chunk.byteLength > this.maxInputSize - this.inputSize) {
            this.destroy();
            throw new CompressError(
                Status.SizeLimit,
                this.algorithm,
                "buffered block exceeds maxInputSize",
            );
        }
        if (chunk.byteLength) {
            this.chunks.push(chunk.slice());
            this.inputSize += chunk.byteLength;
        }
        return new Uint8Array();
    }

    /** Processes exactly one assembled block, releasing input even on codec errors. */
    finish(): Uint8Array {
        this.ensureWritable();
        this.finished = true;
        try {
            const input = new Uint8Array(this.inputSize);
            let offset = 0;
            for (const chunk of this.chunks) {
                input.set(chunk, offset);
                offset += chunk.byteLength;
            }
            return this.processBlock(input);
        } finally {
            this.chunks = [];
            this.inputSize = 0;
        }
    }

    /** Releases buffered input. Repeated destruction is harmless. */
    destroy(): void {
        this.destroyed = true;
        this.chunks = [];
        this.inputSize = 0;
    }

    /** Supports explicit resource management with using declarations. */
    [Symbol.dispose](): void {
        this.destroy();
    }

    /** Rejects operations after completion or destruction. */
    private ensureWritable(): void {
        if (this.destroyed)
            throw new CompressError(
                Status.StreamState,
                this.algorithm,
                "stream has been destroyed",
            );
        if (this.finished)
            throw new CompressError(
                Status.StreamFinished,
                this.algorithm,
                "stream has been finished",
            );
    }
}

/** Wraps one buffered block in the standard Web Streams interface. */
export function createBlockTransform(
    createStream: () => Promise<BufferedBlockStream>,
): TransformStream<Uint8Array, Uint8Array> {
    let stream: BufferedBlockStream;
    return new TransformStream({
        async start() {
            stream = await createStream();
        },
        transform(chunk) {
            try {
                stream.write(chunk);
            } catch (error) {
                stream.destroy();
                throw error;
            }
        },
        flush(controller) {
            try {
                const result = stream.finish();
                if (result.byteLength) controller.enqueue(result);
            } finally {
                stream.destroy();
            }
        },
    });
}
