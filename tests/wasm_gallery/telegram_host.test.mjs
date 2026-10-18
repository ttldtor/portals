import assert from 'node:assert/strict';
import test from 'node:test';
import { createTelegramHost } from '../../examples/wasm_gallery/web/src/telegram_host.js';

function fixture(app) {
  const domEvents = new Map();
  const sdkEvents = new Map();
  const properties = new Map();
  const calls = [];
  const document = {
    hidden: false,
    documentElement: {
      dataset: {},
      style: {
        setProperty: (name, value) => properties.set(name, value),
        removeProperty: (name) => properties.delete(name),
      },
    },
    addEventListener: (name, callback) => domEvents.set(name, callback),
    removeEventListener: (name, callback) => {
      if (domEvents.get(name) === callback) domEvents.delete(name);
    },
  };
  if (app) Object.assign(app, {
    onEvent: (name, callback) => sdkEvents.set(name, callback),
    offEvent: (name, callback) => {
      if (sdkEvents.get(name) === callback) sdkEvents.delete(name);
    },
    ready: () => calls.push('ready'),
    expand: () => calls.push('expand'),
  });
  const options = { window: app ? { Telegram: { WebApp: app } } : {}, document };
  return { options, document, properties, calls, domEvents, sdkEvents };
}

test('standalone works without SDK and follows document visibility', () => {
  const f = fixture();
  const activity = [];
  const host = createTelegramHost((value) => activity.push(value), f.options);
  assert.equal(host.telegram, false);
  assert.equal(typeof host.platform, 'string');
  assert.equal(f.document.documentElement.dataset.host, 'browser');
  host.ready();
  assert.deepEqual(f.calls, []);
  f.document.hidden = true;
  f.domEvents.get('visibilitychange')();
  f.document.hidden = false;
  f.domEvents.get('visibilitychange')();
  assert.deepEqual(activity, [false, true, false]);
  host.dispose();
  assert.equal(f.domEvents.size, 0);
});

test('SDK stub with missing or unknown platform remains a browser host', () => {
  for (const platform of [undefined, '', 'unknown']) {
    const f = fixture({ platform });
    const host = createTelegramHost(() => {}, f.options);
    assert.equal(host.telegram, false);
    assert.equal(f.sdkEvents.size, 0);
    assert.equal(f.document.documentElement.dataset.host, 'browser');
    host.ready();
    assert.deepEqual(f.calls, []);
    host.dispose();
  }
});

test('Telegram ready expands and publishes stable viewport, page theme and insets', () => {
  const app = {
    platform: 'android', viewportStableHeight: 640, colorScheme: 'dark',
    themeParams: { bg_color: '#123456', text_color: '#aBcDEF', hint_color: '#778899', secondary_bg_color: '#334455' },
    safeAreaInset: { top: 24, bottom: 12, left: 0, right: 3 },
    contentSafeAreaInset: { top: 18, bottom: 4, left: 1, right: 2 },
  };
  const f = fixture(app);
  const host = createTelegramHost(() => {}, f.options);
  assert.equal(host.telegram, true);
  assert.equal(host.platform, 'android');
  assert.equal(f.document.documentElement.dataset.host, 'telegram');
  host.ready();
  assert.deepEqual(f.calls, ['ready', 'expand']);
  assert.equal(f.properties.get('--poc-viewport-height'), '640px');
  assert.equal(f.properties.get('--poc-background'), '#123456');
  assert.equal(f.properties.get('--poc-text'), '#aBcDEF');
  assert.equal(f.properties.get('--poc-hint'), '#778899');
  assert.equal(f.properties.get('--poc-border'), '#334455');
  assert.equal(f.document.documentElement.style.colorScheme, 'dark');
  for (const [side, value] of Object.entries(app.safeAreaInset)) {
    assert.equal(f.properties.get(`--poc-safe-${side}`), `${value}px`);
    assert.equal(f.properties.get(`--poc-content-${side}`), `${app.contentSafeAreaInset[side]}px`);
  }
  app.viewportStableHeight = 500;
  f.sdkEvents.get('viewportChanged')({ isStateStable: false });
  assert.equal(f.properties.get('--poc-viewport-height'), '640px');
  f.sdkEvents.get('viewportChanged')({ isStateStable: true });
  assert.equal(f.properties.get('--poc-viewport-height'), '500px');
  for (const height of [0, -1, NaN, Infinity, '800']) {
    app.viewportStableHeight = height;
    f.sdkEvents.get('viewportChanged')({ isStateStable: true });
    assert.equal(f.properties.get('--poc-viewport-height'), '500px');
  }
  app.themeParams = { bg_color: 'red', text_color: '#fff', hint_color: '#000000', secondary_bg_color: '#123456;' };
  app.colorScheme = 'light';
  f.sdkEvents.get('themeChanged')();
  assert.equal(f.properties.has('--poc-background'), false);
  assert.equal(f.properties.has('--poc-text'), false);
  assert.equal(f.properties.has('--poc-border'), false);
  assert.equal(f.properties.get('--poc-hint'), '#000000');
  assert.equal(f.document.documentElement.style.colorScheme, 'light');
  app.themeParams = { bg_color: '#123456\n' };
  f.sdkEvents.get('themeChanged')();
  assert.equal(f.properties.has('--poc-background'), false);
  host.dispose();
});

test('v8 inset events sanitize values and activity combines SDK activation with visibility', () => {
  const app = { platform: 'ios', isActive: true, isVersionAtLeast: (version) => version === '8.0' };
  const f = fixture(app);
  const activity = [];
  const host = createTelegramHost((value) => activity.push(value), f.options);
  app.safeAreaInset = { top: -5, bottom: Infinity, left: 7, right: '4' };
  app.contentSafeAreaInset = { top: 40, bottom: NaN };
  f.sdkEvents.get('safeAreaChanged')();
  f.sdkEvents.get('contentSafeAreaChanged')();
  assert.equal(f.properties.get('--poc-safe-top'), '0px');
  assert.equal(f.properties.get('--poc-safe-bottom'), '0px');
  assert.equal(f.properties.get('--poc-safe-left'), '7px');
  assert.equal(f.properties.get('--poc-safe-right'), '0px');
  assert.equal(f.properties.get('--poc-content-top'), '40px');
  assert.equal(f.properties.get('--poc-content-bottom'), '0px');
  app.isActive = false;
  f.sdkEvents.get('deactivated')();
  f.document.hidden = true;
  f.domEvents.get('visibilitychange')();
  app.isActive = true;
  f.sdkEvents.get('activated')();
  f.document.hidden = false;
  f.domEvents.get('visibilitychange')();
  assert.deepEqual(activity, [false, true, false]);
  host.dispose();
});

test('old SDK subscribes only to basic events and initially inactive hosts suspend', () => {
  const f = fixture({ platform: 'tdesktop', isActive: false, isVersionAtLeast: () => false });
  const activity = [];
  const host = createTelegramHost((value) => activity.push(value), f.options);
  assert.deepEqual([...f.sdkEvents.keys()].sort(), ['themeChanged', 'viewportChanged']);
  assert.deepEqual(activity, [true]);
  host.dispose();
});

test('dispose removes every listener and neutralizes queued callbacks and ready', () => {
  const app = { platform: 'ios', isVersionAtLeast: () => true, viewportStableHeight: 600 };
  const f = fixture(app);
  const activity = [];
  const host = createTelegramHost((value) => activity.push(value), f.options);
  const queued = [...f.sdkEvents.values(), ...f.domEvents.values()];
  const before = [...f.properties];
  host.dispose();
  host.dispose();
  assert.equal(f.sdkEvents.size, 0);
  assert.equal(f.domEvents.size, 0);
  app.isActive = false;
  app.viewportStableHeight = 300;
  f.document.hidden = true;
  for (const callback of queued) callback({ isStateStable: true });
  host.ready();
  assert.deepEqual(activity, [false]);
  assert.deepEqual([...f.properties], before);
  assert.deepEqual(f.calls, []);
});

test('visual viewport tracks the keyboard independently of stable Telegram height and cleans up', () => {
  const f = fixture({ platform: 'android', viewportStableHeight: 640 });
  const events = new Map();
  const viewport = { height: 640,
    addEventListener: (name, callback) => events.set(name, callback),
    removeEventListener: (name) => events.delete(name) };
  f.options.window.visualViewport = viewport;
  const host = createTelegramHost(() => {}, f.options);
  assert.equal(f.properties.get('--poc-visible-height'), '640px');
  const queued = events.get('resize');
  viewport.height = 320;
  queued();
  assert.equal(f.properties.get('--poc-visible-height'), '320px');
  assert.equal(f.properties.get('--poc-viewport-height'), '640px');
  host.dispose();
  assert.equal(events.size, 0);
  viewport.height = 500;
  queued();
  assert.equal(f.properties.get('--poc-visible-height'), '320px');
});
