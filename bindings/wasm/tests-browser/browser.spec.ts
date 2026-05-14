/**
 * Browser smoke test. Boots the bundled consumer page (served by
 * tests-browser/serve.mjs), waits for the in-page round-trips to finish,
 * and asserts every algo reported "ok".
 *
 * Each browser engine (Chromium, Firefox, WebKit) gets its own run via
 * playwright.config.ts projects. Catches regressions in:
 *   - WebAssembly.instantiateStreaming path through fetch (vs. Node fs)
 *   - browser-side WASI shim sufficiency
 *   - bundler asset URL resolution from `new URL("./x.wasm", import.meta.url)`
 */

import { test, expect } from "@playwright/test";

test("all six algorithms round-trip in browser", async ({ page }) => {
    await page.goto("/");
    // serve.mjs sets document.title = "ready" once results are populated.
    await expect(page).toHaveTitle("ready", { timeout: 30_000 });

    const results = await page.evaluate(() => (globalThis as any).__cuResults);
    expect(results).toEqual({
        zstd: "ok",
        brotli: "ok",
        zlib: "ok",
        bz2: "ok",
        lz4: "ok",
        xz: "ok",
    });
});
