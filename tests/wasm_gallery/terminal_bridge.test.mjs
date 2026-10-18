// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
import test from 'node:test';
import assert from 'node:assert/strict';
import { createTerminalBridge } from '../../examples/wasm_gallery/web/src/terminal_bridge.js';

test('owned copies survive source changes, backpressure and late completion', () => {
  const writes = [];
  const terminal = { write(bytes, done) { writes.push({ bytes, done }); } };
  const bridge = createTerminalBridge(terminal, 4);
  const source = new Uint8Array([0xe2, 0x82, 0xac]);
  assert.equal(bridge.write(source), 3);
  source.fill(0);
  assert.deepEqual([...writes[0].bytes], [0xe2, 0x82, 0xac]);
  assert.equal(bridge.write(new Uint8Array([1, 2])), 0);
  assert.equal(writes.length, 1);
  bridge.close();
  writes[0].done();
  assert.equal(bridge.pendingBytes, 0);
  assert.equal(bridge.write(new Uint8Array([1])), -1);
});

test('completed writes release capacity and preserve order', () => {
  const writes = [];
  const bridge = createTerminalBridge({ write(bytes, done) { writes.push({ bytes, done }); } }, 2);
  assert.equal(bridge.write(new Uint8Array([1, 2])), 2);
  assert.equal(bridge.write(new Uint8Array([3])), 0);
  writes[0].done();
  assert.equal(bridge.write(new Uint8Array([3])), 1);
  assert.deepEqual(writes.map(write => [...write.bytes]), [[1, 2], [3]]);
});

test('sink failure is sticky and bytes are not retried', () => {
  let calls = 0;
  const bridge = createTerminalBridge({ write() { calls++; throw new Error('sink failed'); } });
  assert.throws(() => bridge.write(new Uint8Array([1])), /sink failed/);
  assert.equal(bridge.pendingBytes, 0);
  assert.equal(bridge.write(new Uint8Array([1])), -1);
  assert.equal(calls, 1);
});
