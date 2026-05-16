/**
 * Playwright runs ONE test (tests/browser.spec.ts) against three browser
 * engines. The test boots a tiny static server serving a built consumer
 * bundle, navigates a headless browser to it, and asserts the round-trip
 * succeeded by reading a globalThis result.
 *
 * Why a separate config from vitest: Playwright owns the browser
 * lifecycle and assertion vocabulary; vitest owns the Node-side suite.
 * They share zero state.
 */

import { defineConfig, devices } from "@playwright/test";

export default defineConfig({
    testDir: "tests-browser",
    fullyParallel: true,
    forbidOnly: !!process.env.CI,
    retries: process.env.CI ? 2 : 0,
    workers: process.env.CI ? 1 : undefined,
    reporter: process.env.CI ? "line" : "list",
    use: {
        // Local static server spun up by globalSetup.
        baseURL: "http://127.0.0.1:4173",
        trace: "on-first-retry",
    },
    webServer: {
        // tests-browser/serve.mjs builds the consumer bundle + serves it.
        command: "node tests-browser/serve.mjs",
        url: "http://127.0.0.1:4173/index.html",
        reuseExistingServer: !process.env.CI,
        timeout: 60_000,
    },
    projects: [
        { name: "chromium", use: { ...devices["Desktop Chrome"] } },
        { name: "firefox", use: { ...devices["Desktop Firefox"] } },
        { name: "webkit", use: { ...devices["Desktop Safari"] } },
    ],
});
