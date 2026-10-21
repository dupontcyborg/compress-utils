import { defineConfig } from "vitest/config";

export default defineConfig({
    test: {
        // tests/ holds Node-side vitest suites.
        // tests-browser/ is Playwright-only; tests-runtime/ is Deno-only.
        include: ["tests/**/*.test.ts"],
        exclude: ["node_modules", "dist", "tests-browser", "tests-runtime"],
        // `vitest run --coverage`: the TypeScript binding only (the C core has
        // its own coverage). Most tests import the package by name, which runs
        // dist/; dist/ must be included for those runs to count, and tsc's
        // source maps attribute them to src/, the only paths reported.
        // resolve-browser.ts and resolve-default.ts only run in the browser
        // and Deno suites, so they read 0% here.
        coverage: {
            provider: "v8",
            include: ["src/**/*.ts", "dist/**/*.js"],
            reporter: ["text", ["text", { file: "coverage.txt" }], "html"],
            reportsDirectory: "coverage",
        },
    },
});
