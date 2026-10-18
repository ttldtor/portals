import assert from 'node:assert/strict';
import test from 'node:test';
import { createOrientationInput } from '../../examples/wasm_gallery/web/src/orientation_input.js';

function fixture(overrides = {}) {
  const events = new Map();
  const starts = [];
  const states = [];
  const samples = [];
  let stops = 0;
  const sensor = {
    beta: null, gamma: null,
    start(params, callback) { starts.push({ params, callback }); },
    stop(callback) { stops++; callback?.(true); },
  };
  const app = {
    platform: 'android', isVersionAtLeast: (version) => version === '8.0',
    DeviceOrientation: sensor,
    onEvent(name, callback) {
      if (!events.has(name)) events.set(name, new Set());
      events.get(name).add(callback);
    },
    offEvent(name, callback) { events.get(name)?.delete(callback); },
  };
  const window = { Telegram: { WebApp: app }, screen: { orientation: { angle: 0 } } };
  const adapter = createOrientationInput({
    window, onState: (value) => states.push(value), onSample: (value) => samples.push(value), ...overrides,
  });
  const emit = (name, data) => { for (const callback of [...(events.get(name) ?? [])]) callback(data); };
  const listenerCount = () => [...events.values()].reduce((count, set) => count + set.size, 0);
  return { adapter, window, app, sensor, starts, states, samples, emit, listenerCount, stops: () => stops };
}

const settled = () => new Promise((resolve) => queueMicrotask(resolve));

test('resume waits for stop acknowledgement and its event before installing a new lease', async () => {
  const f = fixture();
  let callbacks = [];
  f.sensor.stop = (callback) => { callbacks.push(callback); };
  f.adapter.activate(8); f.starts[0].callback(true); await settled();
  f.adapter.pause(true); f.adapter.pause(false);
  assert.equal(f.starts.length, 1);
  assert.equal(f.listenerCount(), 0);
  assert.equal(callbacks.length, 1);
  for (let index = 0; index < callbacks.length; index++) callbacks[index](true);
  callbacks = [];
  f.emit('deviceOrientationStopped');
  assert.equal(f.starts.length, 1);
  await settled();
  assert.equal(f.starts.length, 2);
  assert.equal(f.listenerCount(), 4);
  f.starts[1].callback(true); await settled();
  Object.assign(f.sensor, { beta: 1, gamma: 2 }); f.emit('deviceOrientationChanged');
  assert.deepEqual(f.samples, [{ session: 8, beta: 1, gamma: 2, angle: 0 }]);
  assert.deepEqual(f.states.at(-1), { session: 8, state: 'waiting' });
  f.adapter.dispose();
});

test('pending stop coalesces switches and cannot restart after disposal', async () => {
  for (const dispose of [false, true]) {
    const f = fixture();
    const callbacks = [];
    f.sensor.stop = (callback) => { callbacks.push(callback); };
    f.adapter.activate(1); f.starts[0].callback(true); await settled();
    for (const session of [2, 3, 4]) f.adapter.activate(session);
    assert.equal(f.starts.length, 1); assert.equal(callbacks.length, 1);
    if (dispose) f.adapter.dispose();
    const stateCount = f.states.length;
    callbacks[0](true); f.emit('deviceOrientationStopped'); await settled();
    assert.equal(f.states.length, stateCount);
    assert.equal(f.starts.length, dispose ? 1 : 2);
    assert.equal(f.listenerCount(), dispose ? 0 : 4);
    if (!dispose) {
      f.starts[1].callback(true);
      Object.assign(f.sensor, { beta: 1, gamma: 2 }); f.emit('deviceOrientationChanged');
      assert.equal(f.samples.at(-1).session, 4);
    }
    f.adapter.dispose();
  }
});

test('late start success during stop waits for both stop acknowledgements', async () => {
  const f = fixture();
  const callbacks = [];
  f.sensor.stop = (callback) => { callbacks.push(callback); };
  f.adapter.activate(1); f.adapter.activate(2);
  f.starts[0].callback(true);
  assert.equal(callbacks.length, 1);
  callbacks[0](true); f.emit('deviceOrientationStopped'); await settled();
  assert.equal(callbacks.length, 2); assert.equal(f.starts.length, 1);
  callbacks[1](true); f.emit('deviceOrientationStopped'); await settled();
  assert.equal(f.starts.length, 2);
  f.starts[1].callback(true);
  Object.assign(f.sensor, { beta: 1, gamma: 2 }); f.emit('deviceOrientationChanged');
  assert.equal(f.samples.at(-1).session, 2);
  f.adapter.dispose();
});

test('unsupported hosts and incomplete SDK report unavailable without starting', () => {
  for (const change of [
    (f) => { delete f.window.Telegram; },
    (f) => { f.app.platform = 'unknown'; },
    (f) => { f.app.isVersionAtLeast = () => false; },
    (f) => { delete f.app.isVersionAtLeast; },
    (f) => { delete f.app.DeviceOrientation; },
    (f) => { delete f.sensor.stop; },
    (f) => { delete f.app.offEvent; },
  ]) {
    const f = fixture(); change(f); f.adapter.activate(7);
    assert.deepEqual(f.states, [{ session: 7, state: 'unavailable' }]);
    assert.equal(f.starts.length, 0); assert.equal(f.listenerCount(), 0);
  }
});

test('starts relative orientation at 50 ms and forwards only finite current samples', () => {
  const f = fixture(); f.adapter.activate(4);
  assert.deepEqual(f.states, [{ session: 4, state: 'waiting' }]);
  assert.deepEqual(f.starts[0].params, { refresh_rate: 50, need_absolute: false });
  Object.assign(f.sensor, { beta: 0.25, gamma: -0.5 });
  f.emit('deviceOrientationChanged'); assert.equal(f.samples.length, 0);
  f.starts[0].callback(true); f.emit('deviceOrientationStarted');
  f.emit('deviceOrientationChanged');
  assert.deepEqual(f.samples, [{ session: 4, beta: 0.25, gamma: -0.5, angle: 0 }]);
  for (const beta of [NaN, Infinity, null, '1']) {
    f.sensor.beta = beta; f.emit('deviceOrientationChanged');
  }
  f.sensor.beta = 1; f.sensor.gamma = NaN; f.emit('deviceOrientationChanged');
  assert.equal(f.samples.length, 1);
});

test('normalizes screen rotations with legacy fallback and safe default', () => {
  const f = fixture(); f.adapter.activate(1); f.starts[0].callback(true);
  Object.assign(f.sensor, { beta: 0, gamma: 0 });
  for (const [input, expected] of [[90, 90], [-90, 270], [450, 90], [180, 180], [37, 0]]) {
    f.window.screen.orientation.angle = input; f.emit('deviceOrientationChanged');
    assert.equal(f.samples.at(-1).angle, expected);
  }
  delete f.window.screen; f.window.orientation = -90; f.emit('deviceOrientationChanged');
  assert.equal(f.samples.at(-1).angle, 270);
  delete f.window.orientation; f.emit('deviceOrientationChanged');
  assert.equal(f.samples.at(-1).angle, 0);
});

test('pause stops and removes listeners; resume starts the selected session again', async () => {
  const f = fixture(); f.adapter.activate(8); f.starts[0].callback(true); await settled();
  f.adapter.pause(true);
  assert.equal(f.stops(), 1); assert.equal(f.listenerCount(), 0);
  assert.deepEqual(f.states.at(-1), { session: 8, state: 'paused' });
  f.adapter.pause(true); assert.equal(f.stops(), 1);
  f.adapter.pause(false); assert.equal(f.starts.length, 1);
  await settled(); assert.equal(f.starts.length, 2); assert.equal(f.listenerCount(), 4);
  assert.deepEqual(f.states.at(-1), { session: 8, state: 'waiting' });
});

test('activation while paused never starts until resumed', () => {
  const f = fixture(); f.adapter.pause(true); f.adapter.activate(2);
  assert.equal(f.starts.length, 0);
  assert.deepEqual(f.states, [{ session: 2, state: 'paused' }]);
  f.adapter.pause(false); assert.equal(f.starts.length, 1);
});

test('pending starts serialize switches without growing SDK callback queues', async () => {
  const f = fixture(); f.adapter.activate(1);
  for (let id = 2; id < 50; id++) f.adapter.activate(id);
  assert.equal(f.starts.length, 1); assert.equal(f.listenerCount(), 0);
  f.starts[0].callback(true); assert.equal(f.starts.length, 1);
  await settled(); assert.equal(f.starts.length, 2); assert.equal(f.listenerCount(), 4);
  f.starts[1].callback(true); Object.assign(f.sensor, { beta: 1, gamma: 2 });
  f.emit('deviceOrientationChanged'); assert.equal(f.samples.at(-1).session, 49);
});

test('captured old handlers and callbacks cannot publish into a replacement session', async () => {
  const f = fixture();
  const handlers = [];
  const original = f.app.onEvent;
  f.app.onEvent = (name, callback) => { handlers.push(callback); original(name, callback); };
  f.adapter.activate(1);
  const oldHandlers = [...handlers];
  f.starts[0].callback(true); await settled();
  f.adapter.activate(2); await settled(); f.starts[1].callback(true); await settled();
  const previousStates = f.states.length;
  const previousStops = f.stops();
  for (const callback of oldHandlers) callback({ error: 'UNSUPPORTED' });
  f.starts[0].callback(true);
  assert.equal(f.states.length, previousStates); assert.equal(f.stops(), previousStops);
  assert.equal(f.listenerCount(), 4); assert.equal(f.samples.length, 0);
  f.adapter.deactivate();
  const count = f.states.length;
  for (const callback of handlers) callback({ error: 'UNSUPPORTED' });
  f.starts[0].callback(true);
  assert.equal(f.states.length, count); assert.equal(f.samples.length, 0);
  assert.equal(f.listenerCount(), 0);
});

test('late pending success after deactivation or disposal stops without emitting', async () => {
  for (const operation of ['deactivate', 'dispose']) {
    const f = fixture(); f.adapter.activate(3); f.adapter[operation]();
    const count = f.states.length; const previousStops = f.stops();
    f.starts[0].callback(true); await settled();
    assert.equal(f.stops(), previousStops + 1); assert.equal(f.states.length, count);
    assert.equal(f.starts.length, 1); assert.equal(f.listenerCount(), 0);
    f.adapter.dispose(); f.adapter.activate(4); f.adapter.pause(false);
    assert.equal(f.starts.length, 1);
  }
});

test('reported failures clean up and stale late success cannot resurrect the lease', async () => {
  const f = fixture(); f.adapter.activate(5);
  f.emit('deviceOrientationFailed', { error: 'UNSUPPORTED' });
  assert.deepEqual(f.states.at(-1), { session: 5, state: 'unavailable' });
  assert.equal(f.listenerCount(), 0);
  f.starts[0].callback(true); await settled();
  assert.equal(f.starts.length, 1); assert.equal(f.samples.length, 0);
});

test('negative start callback and unexpected stop report unavailable', async () => {
  const f = fixture(); f.adapter.activate(1); f.starts[0].callback(false); await settled();
  assert.deepEqual(f.states.at(-1), { session: 1, state: 'unavailable' });
  f.adapter.activate(2); f.starts[1].callback(true); await settled();
  f.emit('deviceOrientationStopped');
  assert.deepEqual(f.states.at(-1), { session: 2, state: 'unavailable' });
  assert.equal(f.listenerCount(), 0);
});

test('throwing SDK methods do not escape or retain listeners', () => {
  for (const method of ['start', 'stop']) {
    const f = fixture(); f.sensor[method] = () => { throw Error('SDK failure'); };
    assert.doesNotThrow(() => { f.adapter.activate(1); if (method === 'stop') f.adapter.pause(true); });
    assert.equal(f.listenerCount(), 0);
    assert.deepEqual(f.states.at(-1), { session: 1, state: 'unavailable' });
  }
});

test('throwing consumer callbacks stop tracking instead of escaping through the SDK', () => {
  const f = fixture({ onSample() { throw Error('WASM failure'); } });
  f.adapter.activate(1); f.starts[0].callback(true);
  Object.assign(f.sensor, { beta: 1, gamma: 2 });
  assert.doesNotThrow(() => f.emit('deviceOrientationChanged'));
  assert.equal(f.listenerCount(), 0);
  assert.deepEqual(f.states.at(-1), { session: 1, state: 'unavailable' });
  const g = fixture({ onState() { throw Error('WASM failure'); } });
  assert.doesNotThrow(() => g.adapter.activate(2));
  assert.equal(g.listenerCount(), 0); assert.equal(g.starts.length, 0);
});

test('reentrant state callbacks can deactivate or replace a session safely', async () => {
  let adapter;
  const f = fixture({ onState({ session }) { if (session === 1) adapter.activate(2); } });
  adapter = f.adapter; adapter.activate(1);
  assert.equal(f.starts.length, 1);
  f.starts[0].callback(true); await settled();
  Object.assign(f.sensor, { beta: 1, gamma: 2 }); f.emit('deviceOrientationChanged');
  assert.equal(f.samples.at(-1).session, 2);
  const g = fixture({ onState() { g.adapter.deactivate(); } });
  g.adapter.activate(1); assert.equal(g.starts.length, 0); assert.equal(g.listenerCount(), 0);
});

test('synchronous SDK completions remain safe during deactivate, dispose, and failure', async () => {
  for (const operation of ['deactivate', 'dispose', 'failure']) {
    const f = fixture();
    f.sensor.start = (params, callback) => {
      if (operation === 'failure') f.emit('deviceOrientationFailed', { error: 'UNSUPPORTED' });
      else f.adapter[operation]();
      callback(true);
    };
    assert.doesNotThrow(() => f.adapter.activate(9));
    await settled();
    assert.equal(f.listenerCount(), 0); assert.equal(f.samples.length, 0);
    assert.equal(f.stops(), 2);
  }
});

test('restart is deferred until Telegram has drained the old callback array', async () => {
  const f = fixture();
  let callbacks = [];
  let starts = 0;
  f.sensor.start = (params, callback) => { starts++; callbacks.push(callback); };
  const nativeStarted = () => {
    for (let index = 0; index < callbacks.length; index++) callbacks[index](true);
    callbacks = [];
    f.emit('deviceOrientationStarted');
  };
  f.adapter.activate(1); f.adapter.activate(2);
  assert.equal(starts, 1); assert.equal(callbacks.length, 1);
  nativeStarted(); assert.equal(starts, 1);
  await settled(); assert.equal(starts, 2); assert.equal(callbacks.length, 1);
  nativeStarted(); await settled();
  assert.equal(callbacks.length, 0);
  Object.assign(f.sensor, { beta: 1, gamma: 2 }); f.emit('deviceOrientationChanged');
  assert.deepEqual(f.samples, [{ session: 2, beta: 1, gamma: 2, angle: 0 }]);
});
