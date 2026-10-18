// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
// Browser tests live outside web/; resolve their runner from its pinned package.
import { createRequire } from 'node:module';
const require = createRequire(new URL('../../examples/wasm_gallery/web/package.json', import.meta.url));
export const { test, expect, defineConfig } = require('@playwright/test');
