// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0

const TAP_SLOP_CSS_PIXELS = 10;
const TAP_MAX_DURATION_MS = 500;

export function createTouchInput(terminal, sendInput, isActive) {
  const screen = terminal.element.querySelector('.xterm-screen');
  let pending = null;

  function cellAt(event) {
    const rect = screen.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;
    if (rect.width <= 0 || rect.height <= 0 || x < 0 || y < 0 || x >= rect.width || y >= rect.height) return null;
    return { column: Math.floor(x * terminal.cols / rect.width) + 1,
      row: Math.floor(y * terminal.rows / rect.height) + 1 };
  }

  function cancel() {
    const previous = pending;
    pending = null;
    if (previous && screen.hasPointerCapture(previous.id)) screen.releasePointerCapture(previous.id);
  }

  function down(event) {
    if (event.pointerType !== 'touch' && event.pointerType !== 'pen') return;
    // Suppress compatibility mouse events, or one tap could activate twice.
    event.preventDefault();
    if (!isActive() || !event.isPrimary || pending || event.button !== 0) {
      cancel();
      return;
    }
    const cell = cellAt(event);
    if (!cell) return;
    pending = { id: event.pointerId, x: event.clientX, y: event.clientY, cell, startedAt: event.timeStamp };
    screen.setPointerCapture(event.pointerId);
  }

  function move(event) {
    if (pending?.id !== event.pointerId) return;
    if (Math.hypot(event.clientX - pending.x, event.clientY - pending.y) > TAP_SLOP_CSS_PIXELS) cancel();
  }

  function up(event) {
    if (pending?.id !== event.pointerId) return;
    const previous = pending;
    const cell = cellAt(event);
    cancel();
    event.preventDefault();
    if (!isActive() || !cell || event.timeStamp - previous.startedAt > TAP_MAX_DURATION_MS ||
        cell.column !== previous.cell.column || cell.row !== previous.cell.row ||
        Math.hypot(event.clientX - previous.x, event.clientY - previous.y) > TAP_SLOP_CSS_PIXELS) return;
    terminal.focus();
    sendInput(`\x1b[<0;${cell.column};${cell.row}M\x1b[<0;${cell.column};${cell.row}m`);
  }

  function interrupted(event) {
    if (pending?.id === event.pointerId) cancel();
  }

  function anotherPointer(event) {
    if (pending && event.pointerType !== 'mouse' && event.pointerId !== pending.id) cancel();
  }

  const handlers = { pointerdown: down, pointermove: move, pointerup: up,
    pointercancel: interrupted, lostpointercapture: interrupted };
  for (const [name, handler] of Object.entries(handlers)) screen.addEventListener(name, handler, { capture: true, passive: false });
  screen.ownerDocument.addEventListener('pointerdown', anotherPointer, { capture: true });

  return { cancel, dispose() {
    cancel();
    for (const [name, handler] of Object.entries(handlers)) screen.removeEventListener(name, handler, { capture: true });
    screen.ownerDocument.removeEventListener('pointerdown', anotherPointer, { capture: true });
  } };
}
