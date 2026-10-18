// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const builder = fileURLToPath(new URL('../../examples/wasm_gallery/web/build.mjs', import.meta.url));
test('host build help works without WASM artifacts and describes output and exit codes', () => {
  const result = spawnSync(process.execPath, [builder, '--help'], { encoding: 'utf8' });
  assert.equal(result.status, 0);
  assert.match(result.stdout, /Requires:.*portals-gallery\.js/);
  assert.match(result.stdout, /Writes:.*provenance\.json/);
  assert.match(result.stdout, /Exit codes: 0.*1.*2/);
});
test('host builder rejects unknown switches before trying to read the runtime', () => {
  const result = spawnSync(process.execPath, [builder, '--unknown'], { encoding: 'utf8' });
  assert.equal(result.status, 2);
  assert.match(result.stderr, /Invalid arguments/);
});
