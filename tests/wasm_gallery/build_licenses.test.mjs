// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const builder = fileURLToPath(new URL('../../examples/wasm_gallery/web/build.mjs', import.meta.url));
const license = new URL('../../vendor/textual/LICENSE', import.meta.url);

test('web bundle retains the complete Textual palette notice and indexes it', async () => {
  const output = await mkdtemp(join(tmpdir(), 'portals-web-licenses-'));
  try {
    // The host packager only hashes these inputs; no WASM compiler or runtime is needed.
    await writeFile(join(output, 'portals-gallery.js'), '// packaging fixture\n');
    await writeFile(join(output, 'portals-gallery.wasm'), new Uint8Array());
    const result = spawnSync(process.execPath, [builder, output], { encoding: 'utf8' });
    assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
    assert.deepEqual(await readFile(join(output, 'TEXTUAL-LICENSE.txt')), await readFile(license));
    assert.match(await readFile(join(output, 'LICENSES.txt'), 'utf8'), /Textual.*TEXTUAL-LICENSE\.txt/);
    assert.equal(await readFile(join(output, 'portals-gallery.js'), 'utf8'), '// packaging fixture\n');
  } finally {
    await rm(output, { recursive: true, force: true });
  }
});
