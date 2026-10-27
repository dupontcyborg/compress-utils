import { execFileSync } from "node:child_process";
import { describe, expect, it } from "vitest";

const [major, minor] = process.versions.node.split(".").map(Number);
const supportsSourceImports = major! > 24 || (major === 24 && minor! >= 5);
const packageDirectory = new URL("..", import.meta.url);

/** Run Node without Vitest's module resolver so package import conditions are exercised. */
function runNode(source: string, optIn: boolean): string {
    return execFileSync(
        process.execPath,
        [
            ...(optIn ? ["--conditions=compress-utils-wasm-source"] : []),
            "--input-type=module",
            "--eval",
            source,
        ],
        { cwd: packageDirectory, encoding: "utf8" },
    );
}

describe("Node WASM source imports", () => {
    it("keeps byte loading as the default", () => {
        const result = runNode(
            `
            import {resolveWasm} from '#resolver';
            const source = await resolveWasm(new URL('./dist/algorithms/zstd/zstd.wasm', import.meta.url));
            if (!ArrayBuffer.isView(source)) throw new Error('expected bytes');
            console.log('bytes');
        `,
            false,
        );
        expect(result.trim()).toBe("bytes");
    });

    it.skipIf(!supportsSourceImports)(
        "imports a module and round-trips Zstandard when opted in",
        () => {
            const result = runNode(
                `
            import assert from 'node:assert/strict';
            import {resolveWasm} from '#resolver';
            import {compress, decompress} from './dist/algorithms/zstd/index.js';
            const source = await resolveWasm(new URL('./dist/algorithms/zstd/zstd.wasm', import.meta.url));
            assert(source instanceof WebAssembly.Module);
            const input = new TextEncoder().encode('source-phase WASM round-trip');
            assert.deepEqual(await decompress(await compress(input)), input);
            console.log('round-trip');
        `,
                true,
            );
            expect(result.trim()).toBe("round-trip");
        },
    );

    it.skipIf(!supportsSourceImports)("propagates a missing WASM file", () => {
        const result = runNode(
            `
            import assert from 'node:assert/strict';
            import {resolveWasm} from '#resolver';
            await assert.rejects(resolveWasm(new URL('./missing.wasm', import.meta.url)), {code: 'ERR_MODULE_NOT_FOUND'});
            console.log('missing');
        `,
            true,
        );
        expect(result.trim()).toBe("missing");
    });
});
