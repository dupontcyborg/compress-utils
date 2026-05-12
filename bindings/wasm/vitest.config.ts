import { defineConfig } from "vitest/config";

export default defineConfig({
    test: {
        // tests/ holds Node-side vitest suites.
        // tests-browser/ is Playwright-only; tests-runtime/ is Deno-only.
        include: ["tests/**/*.test.ts"],
        exclude: ["node_modules", "dist", "tests-browser", "tests-runtime"],
    },
});
