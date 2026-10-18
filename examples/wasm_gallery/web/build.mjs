// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
// npm run build -- [output-directory]: bundle the host beside a built WASM runtime.
// Reads src/, index.html, runtime artifacts and licenses; missing inputs fail the build.
import { build } from 'esbuild';
import { copyFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
if (args.length === 1 && ['--help', '-h'].includes(args[0])) {
  console.log(`Usage: npm run build -- [output-directory]
Bundle the Gallery/Level browser host beside an existing Emscripten build.
Default: build/wasm-debug/wasm-gallery relative to the repository root.
Requires: npm ci, portals-gallery.js and portals-gallery.wasm in the output directory.
Writes: main.js/main.css and source maps, index.html, provenance.json and licenses.
The C++ runtime artifacts are preserved. Uses content hashes for cache keys.
Exit codes: 0 success/help, 1 build or input error, 2 invalid arguments.`);
  process.exit(0);
}
if (args.length > 1 || args.some(arg => arg.startsWith('-'))) {
  console.error('Invalid arguments. Use npm run build -- --help.');
  process.exit(2);
}

const output = resolve(args[0] || resolve(here, '../../../build/wasm-debug/wasm-gallery'));
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const loaderHash = hash(await readFile(resolve(output, 'portals-gallery.js')));
const wasmHash = hash(await readFile(resolve(output, 'portals-gallery.wasm')));
const runtimeVersion = hash(loaderHash + wasmHash);
await mkdir(output, { recursive: true });
await build({ entryPoints: [resolve(here, 'src/main.js')], bundle: true, format: 'esm',
  outfile: resolve(output, 'main.js'), external: ['./portals-gallery.js'], sourcemap: true,
  define: { __PORTALS_RUNTIME_VERSION__: JSON.stringify(runtimeVersion) } });

let html = await readFile(resolve(here, 'index.html'), 'utf8');
for (const [file, attribute] of [['main.js', 'src'], ['main.css', 'href']]) {
  const digest = hash(await readFile(resolve(output, file)));
  const reference = new RegExp(`${attribute}="\\./${file.replaceAll('.', '\\.')}(?:\\?v=[a-f0-9]+)?"`);
  html = html.replace(reference, `${attribute}="./${file}?v=${digest}"`);
}
await writeFile(resolve(output, 'index.html'), html);
await writeFile(resolve(output, 'provenance.json'), JSON.stringify({
  runtime_version: runtimeVersion,
  portals_gallery_loader_sha256: loaderHash,
  portals_gallery_wasm_sha256: wasmHash
}, null, 2) + '\n');

for (const [source, destination] of [
  ['../LICENSES.txt', 'LICENSES.txt'],
  ['../../../LICENSE.txt', 'PORTALS-LICENSE.txt'],
  ['../../../vendor/utf8proc/LICENSE.md', 'UTF8PROC-LICENSE.md'],
  ['node_modules/@xterm/xterm/LICENSE', 'XTERM-LICENSE.txt'],
  ['node_modules/@xterm/addon-fit/LICENSE', 'XTERM-FIT-LICENSE.txt']
]) await copyFile(resolve(here, source), resolve(output, destination));
