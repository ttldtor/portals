// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
// Playwright runs this helper from the repository root and stops its owned server.
// PORTALS_WASM_DIR selects built assets; PORTALS_WASM_PORT defaults to41813.
// Files are served under a project prefix with WASM MIME and no-store; errors return404.
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
const root = resolve(process.env.PORTALS_WASM_DIR || 'build/wasm-debug/wasm-gallery');
const prefix = '/portals-wasm-poc/';
const port = Number(process.env.PORTALS_WASM_PORT || 41813);
const types = { '.html': 'text/html; charset=utf-8', '.js': 'application/javascript',
  '.css': 'text/css; charset=utf-8', '.wasm': 'application/wasm', '.map': 'application/json', '.json': 'application/json' };
const server = http.createServer(async (request, response) => {
  try {
    const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
    if (!pathname.startsWith(prefix)) { response.writeHead(404).end(); return; }
    const file = resolve(root, pathname.slice(prefix.length) || 'index.html');
    if (!file.startsWith(root + sep)) { response.writeHead(403).end(); return; }
    const bytes = await readFile(file);
    response.writeHead(200, { 'Content-Type': types[extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
    response.end(bytes);
  } catch { response.writeHead(404).end('Not found'); }
});
server.listen(port, '127.0.0.1', () => console.log(`Gallery/Level: http://127.0.0.1:${port}${prefix}`));
for (const signal of ['SIGTERM', 'SIGINT']) process.on(signal, () => server.close(() => process.exit(0)));
