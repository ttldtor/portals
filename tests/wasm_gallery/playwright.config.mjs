// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from './playwright.mjs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const port = process.env.PORTALS_WASM_PORT || '41813';
export default defineConfig({
  testDir: '.', testMatch: '*.spec.mjs', workers: 1, timeout: 30000, reporter: 'line',
  outputDir: resolve(root, 'build/browser-results'),
  use: { browserName: 'chromium', headless: true, baseURL: `http://127.0.0.1:${port}/portals-wasm-poc/`,
    viewport: { width: 1100, height: 800 }, screenshot: 'only-on-failure', trace: 'retain-on-failure' },
  webServer: { command: 'node tests/wasm_gallery/server.mjs', cwd: root,
    url: `http://127.0.0.1:${port}/portals-wasm-poc/`, reuseExistingServer: false, timeout: 10000 }
});
