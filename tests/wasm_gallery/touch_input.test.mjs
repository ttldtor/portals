// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import test from 'node:test';
import assert from 'node:assert/strict';
import { createTouchInput } from '../../examples/wasm_gallery/web/src/touch_input.js';

function setup() {
  const screen = new EventTarget();
  const document = new EventTarget();
  screen.ownerDocument = document;
  const captures = new Set();
  let rect = { left: 10, top: 20, width: 320, height: 200 };
  screen.getBoundingClientRect = () => rect;
  screen.setPointerCapture = id => captures.add(id);
  screen.hasPointerCapture = id => captures.has(id);
  screen.releasePointerCapture = id => captures.delete(id);
  const writes = [];
  let focused = 0;
  let active = true;
  const terminal = { element: { querySelector: () => screen }, cols: 32, rows: 10,
    focus() { focused++; } };
  const adapter = createTouchInput(terminal, text => writes.push(text), () => active);
  function fire(type, extra = {}) {
    const event = new Event(type, { cancelable: true });
    const { timeStamp, ...properties } = extra;
    Object.assign(event, { pointerType: 'touch', pointerId: 1, isPrimary: true,
      button: 0, clientX: 35, clientY: 90 }, properties);
    if (timeStamp !== undefined) Object.defineProperty(event, 'timeStamp', { value: timeStamp });
    screen.dispatchEvent(event);
    return event;
  }
  return { fire, writes, captures, adapter, terminal, get focused() { return focused; },
    outsideDown() {
      const event = new Event('pointerdown');
      Object.assign(event, { pointerType: 'touch', pointerId: 2, isPrimary: false });
      document.dispatchEvent(event);
    },
    resize(value) { rect = value; }, deactivate() { active = false; } };
}

test('touch/pen taps use current cell geometry and one press/release pair; mouse is left to xterm', () => {
  const s = setup();
  assert.equal(s.fire('pointerdown').defaultPrevented, true);
  s.fire('pointerup');
  assert.deepEqual(s.writes, ['\x1b[<0;3;4M\x1b[<0;3;4m']);
  assert.equal(s.focused, 1);
  assert.equal(s.captures.size, 0);
  s.resize({ left: 15, top: 10, width: 640, height: 400 });
  s.fire('pointerdown', { pointerType: 'pen' });
  s.fire('pointerup', { pointerType: 'pen' });
  assert.equal(s.writes[1], '\x1b[<0;2;3M\x1b[<0;2;3m');
  assert.equal(s.fire('pointerdown', { pointerType: 'mouse' }).defaultPrevented, false);
  s.fire('pointerup', { pointerType: 'mouse' });
  assert.equal(s.writes.length, 2);
});

test('a second finger outside the terminal cancels the pending tap', () => {
  const s = setup();
  s.fire('pointerdown');
  s.outsideDown();
  s.fire('pointerup');
  assert.deepEqual(s.writes, []);
  assert.equal(s.captures.size, 0);
});

test('a stationary long press cannot activate a control', () => {
  const s = setup();
  s.fire('pointerdown', { timeStamp: 1000 });
  s.fire('pointerup', { timeStamp: 1600 });
  assert.deepEqual(s.writes, []);
  s.fire('pointerdown', { timeStamp: 2000 });
  s.fire('pointerup', { timeStamp: 2100 });
  assert.equal(s.writes.length, 1);
});

test('movement, another finger, cancellation and leaving the viewport cannot click', () => {
  for (const interrupt of [
    s => s.fire('pointermove', { clientX: 80 }),
    s => s.fire('pointerdown', { pointerId: 2, isPrimary: false }),
    s => s.fire('pointercancel'),
    s => s.fire('lostpointercapture'),
    s => s.adapter.cancel(),
    s => s.deactivate()
  ]) {
    const s = setup();
    s.fire('pointerdown');
    interrupt(s);
    s.fire('pointerup');
    assert.deepEqual(s.writes, []);
    assert.equal(s.captures.size, 0);
  }
  const s = setup();
  s.fire('pointerdown', { clientX: 11 });
  s.fire('pointerup', { clientX: 9 });
  assert.deepEqual(s.writes, []);
});

test('dispose clears a pending capture and removes listeners', () => {
  const s = setup();
  s.fire('pointerdown');
  s.adapter.dispose();
  assert.equal(s.captures.size, 0);
  s.fire('pointerup');
  s.fire('pointerdown');
  s.fire('pointerup');
  assert.deepEqual(s.writes, []);
  assert.equal(s.focused, 0);
});
