// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';
import './style.css';
import { createTerminalBridge } from './terminal_bridge.js';
import { createTelegramHost } from './telegram_host.js';
import { createTouchInput } from './touch_input.js';
import { createOrientationInput } from './orientation_input.js';

const DEFAULT_FONT_SIZE = 18;
const MIN_FONT_SIZE = 10;
const APPS = [
  { name: 'Gallery', title: 'Portals gallery', columns: 22, rows: 11 },
  { name: 'Level', title: 'Phone level', columns: 25, rows: 19 }
];
const RUNTIME_VERSION = __PORTALS_RUNTIME_VERSION__;
const status = document.querySelector('#status');
const container = document.querySelector('#terminal');
let module;
let active = false;
let closed = false;
let suspended = false;
let frame = null;
let touchInput;
let orientation;
let selectedApp = 0;
let currentApp = -1;
let session = 0;
let switching = false;
const appButtons = [...document.querySelectorAll('[data-app]')];
const host = createTelegramHost(setSuspended);
document.querySelector('#environment').textContent = host.telegram ? `Telegram · ${host.platform}` : 'Browser preview';
const terminal = new Terminal({ cursorBlink: false, scrollback: 0, fontSize: DEFAULT_FONT_SIZE, screenReaderMode: true });
const fit = new FitAddon();
terminal.loadAddon(fit);
terminal.open(container);
terminal.attachCustomKeyEventHandler(event => {
  // Screen reader mode leaves Shift+Tab's browser default enabled in xterm.
  // Keep navigation in the scene while xterm sends its usual Tab/CSI Z bytes.
  if (active && !suspended && event.type === 'keydown' && event.key === 'Tab' &&
      !event.ctrlKey && !event.altKey && !event.metaKey) event.preventDefault();
  return true;
});
// Alternate screen prevents xterm's scrollback reflow from shifting TUI rows on resize.
// Portals paints its own focus; hide xterm's independent terminal cursor (DECTCEM).
terminal.write('\x1b[?1049h\x1b[?25l');
resizeTerminal();

const bridge = createTerminalBridge(terminal);
const encoder = new TextEncoder();
const decoder = new TextDecoder();
orientation = createOrientationInput({
  onSample: sample => sensorCall(sample.session, () =>
    module._portals_orientation(sample.session, sample.beta, sample.gamma, sample.angle)),
  onState: update => sensorCall(update.session, () =>
    module._portals_sensor_state(update.session, { unavailable: 0, waiting: 1, paused: 2 }[update.state]))
});
orientation.pause(suspended);
let listeners = [];
let resizeObserver;

function scheduleStep() {
  if (active && !suspended && frame === null) frame = requestAnimationFrame(step);
}

function setSuspended(value) {
  suspended = value;
  orientation?.pause(value);
  touchInput?.cancel();
  if (frame !== null) cancelAnimationFrame(frame);
  frame = null;
  scheduleStep();
}

function messageFromWasm() {
  const pointer = module._portals_error();
  const memory = module.HEAPU8;
  const end = memory.indexOf(0, pointer);
  return decoder.decode(memory.subarray(pointer, end < 0 ? memory.length : end));
}

function finish(error) {
  if (closed) return;
  closed = true;
  active = false;
  if (frame !== null) cancelAnimationFrame(frame);
  frame = null;
  orientation?.dispose();
  appButtons.forEach(button => { button.disabled = true; });
  host.dispose();
  resizeObserver?.disconnect();
  listeners.forEach(listener => listener.dispose());
  listeners = [];
  terminal.write('\x1b[?1000l\x1b[?1006l');
  // Queued xterm writes own their copies and may complete after C++ teardown.
  bridge.close();
  module?._portals_shutdown();
  status.dataset.state = error ? 'error' : 'finished';
  status.textContent = error ? `Error: ${error}` : 'Finished. Reload the page to restart.';
}

function step() {
  frame = null;
  if (!active || suspended) return;
  try {
    const result = module._portals_step();
    if (result < 0) {
      finish(messageFromWasm());
      return;
    }
    syncScene();
    if (result === 1) {
      active = false;
      switching = false;
      session = 0;
      currentApp = -1;
      orientation.deactivate();
      touchInput?.cancel();
      status.dataset.state = 'finished';
      status.textContent = 'Finished. Choose Gallery or Level to start.';
      return;
    }
    scheduleStep();
  } catch (error) {
    finish(error.message || String(error));
  }
}

function sensorCall(tag, callback) {
  if (closed || !active || switching || currentApp !== 1 || tag !== session) return;
  try {
    if (callback() < 0) finish(messageFromWasm());
  } catch (error) { finish(error.message || String(error)); }
}

function syncScene() {
  const app = module._portals_app();
  const nextSession = module._portals_session();
  if (app < 0 || nextSession === session) return;
  currentApp = app;
  session = nextSession;
  switching = false;
  terminal.textarea.readOnly = app === 1;
  terminal.textarea.inputMode = app === 1 ? 'none' : 'text';
  document.querySelector('h1').textContent = APPS[app].title;
  container.setAttribute('aria-label', `Portals terminal ${APPS[app].name.toLowerCase()}`);
  status.dataset.state = 'running';
  status.textContent = 'Tab / Shift+Tab: focus · Enter: activate · F2: theme · F10: quit.';
  if (app === 1) orientation.activate(session);
  else { orientation.deactivate(); terminal.focus(); }
}

function selectApp(app) {
  if (closed || !module || (active && !switching && currentApp === app)) return;
  try {
    orientation.deactivate();
    touchInput?.cancel();
    switching = true;
    session = 0;
    selectedApp = app;
    if (app === 1) terminal.blur();
    appButtons.forEach((button, index) => button.setAttribute('aria-pressed', String(index === app)));
    if (module._portals_select_app(app) < 0) { finish(messageFromWasm()); return; }
    active = true;
    resizeTerminal();
    scheduleStep();
  } catch (error) { finish(error.message || String(error)); }
}

async function start() {
  const { default: createGallery } = await import(runtimeUrl('./portals-gallery.js'));
  if (closed) return;
  module = await createGallery({ noInitialRun: true, locateFile: runtimeUrl,
    portalsWrite: bytes => bridge.write(bytes),
    // Clear only after C++ has retired the old scene; never reenter WASM here.
    portalsClear: () => terminal.write('\x1b[2J\x1b[H') });
  if (closed) {
    module._portals_shutdown();
    return;
  }
  if (module._portals_start(terminal.cols, terminal.rows) !== 0) {
    throw new Error(messageFromWasm());
  }

  active = true;
  syncScene();
  appButtons.forEach((button, app) => {
    button.disabled = false;
    const click = () => selectApp(app);
    button.addEventListener('click', click);
    listeners.push({ dispose: () => button.removeEventListener('click', click) });
  });
  listeners.push(terminal.onData(sendInput));
  touchInput = createTouchInput(terminal, sendInput, () => active && !suspended && !switching);
  listeners.push(touchInput);
  // Normal mouse press/release reporting with SGR cell coordinates.
  terminal.write('\x1b[?1000h\x1b[?1006h');
  listeners.push(terminal.onResize(({ cols, rows }) => {
    try {
      touchInput.cancel();
      if (!closed && module._portals_resize(cols, rows) < 0) finish(messageFromWasm());
    } catch (error) {
      finish(error.message || String(error));
    }
  }));
  resizeObserver = new ResizeObserver(() => { if (!closed) resizeTerminal(); });
  resizeObserver.observe(container);
  terminal.focus();
  host.ready();
  scheduleStep();
}

function runtimeUrl(file) {
  const url = new URL(file, import.meta.url);
  url.searchParams.set('v', RUNTIME_VERSION);
  return url.href;
}

function resizeTerminal() {
  terminal.options.fontSize = DEFAULT_FONT_SIZE;
  const available = fit.proposeDimensions();
  if (!available) return;
  const app = APPS[selectedApp];
  const scale = Math.min(1, available.cols / app.columns, available.rows / app.rows);
  terminal.options.fontSize = Math.max(MIN_FONT_SIZE, Math.floor(DEFAULT_FONT_SIZE * scale));
  fit.fit();
}

function sendInput(text) {
  if (!active || suspended || switching) return;
  try {
    const bytes = encoder.encode(text);
    const pointer = module._malloc(bytes.length || 1);
    if (!pointer) { finish('Cannot allocate input storage.'); return; }
    let result;
    try {
      module.HEAPU8.set(bytes, pointer);
      result = module._portals_input(pointer, bytes.length);
    } finally {
      module._free(pointer);
    }
    if (result < 0) finish(messageFromWasm());
  } catch (error) {
    finish(error.message || String(error));
  }
}

window.addEventListener('pagehide', () => finish(), { once: true });
try {
  await start();
} catch (error) {
  finish(error.message || String(error));
}
