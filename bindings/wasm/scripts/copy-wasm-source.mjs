import {copyFile, mkdir} from 'node:fs/promises';

await mkdir(new URL('../dist/wasm-source/', import.meta.url), {recursive: true});
await copyFile(new URL('../src/wasm-source/resolve-node-source.mjs', import.meta.url), new URL('../dist/wasm-source/resolve-node-source.mjs', import.meta.url));
