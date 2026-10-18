// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
export function createOrientationInput({ window = globalThis.window, onSample, onState }) {
  let selected = null;
  let paused = false;
  let disposed = false;
  let lease = null;
  let pending = null;
  let stopping = null;
  let settling = false;
  let failed = false;

  function supported() {
    const app = window?.Telegram?.WebApp;
    try {
      return app?.platform && app.platform !== 'unknown' && app.isVersionAtLeast?.('8.0') &&
        typeof app.onEvent === 'function' && typeof app.offEvent === 'function' &&
        typeof app.DeviceOrientation?.start === 'function' && typeof app.DeviceOrientation.stop === 'function' ? app : null;
    } catch { return null; }
  }

  function current(owner) {
    return !disposed && !paused && lease === owner && selected === owner.session;
  }

  function notify(session, state) {
    if (disposed || selected !== session) return;
    try { onState({ session, state }); }
    catch {
      failed = true;
      release();
    }
  }

  function stop(sensor) {
    if (stopping) { stopping.repeat = true; return true; }
    const request = { repeat: false, completed: false };
    stopping = request;
    try {
      sensor.stop((success) => {
        if (stopping !== request || request.completed) return;
        request.completed = true;
        // The SDK drains stop callbacks and emits Stopped before a replacement may subscribe.
        queueMicrotask(() => {
          if (stopping !== request) return;
          stopping = null;
          if (!success || (request.repeat && !stop(sensor))) {
            failed = true;
            if (selected !== null) notify(selected, 'unavailable');
          }
          start();
        });
      });
      return true;
    } catch {
      if (stopping === request) stopping = null;
      return false;
    }
  }

  function release() {
    const owner = lease;
    lease = null;
    if (!owner) return true;
    for (const [name, handler] of owner.listeners) {
      try { owner.app.offEvent(name, handler); } catch {}
    }
    return stop(owner.sensor);
  }

  function unavailable(owner) {
    if (!current(owner)) return;
    failed = true;
    release();
    notify(owner.session, 'unavailable');
  }

  function angle() {
    const value = window?.screen?.orientation?.angle ?? window?.orientation ?? 0;
    if (!Number.isFinite(value)) return 0;
    const normalized = ((value % 360) + 360) % 360;
    return [0, 90, 180, 270].includes(normalized) ? normalized : 0;
  }

  function start() {
    if (disposed || paused || selected === null || failed || lease || pending || stopping || settling) return;
    const app = supported();
    if (!app) { failed = true; notify(selected, 'unavailable'); return; }
    const owner = { session: selected, app, sensor: app.DeviceOrientation, listeners: [], started: false };
    lease = owner;
    const handlers = {
      deviceOrientationStarted() { owner.started = true; },
      deviceOrientationStopped() { unavailable(owner); },
      deviceOrientationFailed() { unavailable(owner); },
      deviceOrientationChanged() {
        if (!owner.started) return;
        const { beta, gamma } = owner.sensor;
        if (!Number.isFinite(beta) || !Number.isFinite(gamma)) return;
        try { onSample({ session: owner.session, beta, gamma, angle: angle() }); }
        catch { unavailable(owner); }
      },
    };
    try {
      for (const [name, handler] of Object.entries(handlers)) {
        const guarded = () => { if (current(owner)) handler(); };
        owner.listeners.push([name, guarded]);
        app.onEvent(name, guarded);
        if (!current(owner)) return;
      }
      const request = { owner };
      pending = request;
      owner.sensor.start({ refresh_rate: 50, need_absolute: false }, (success) => {
        if (pending !== request) return;
        // Telegram drains its callback array after invoking callbacks. Defer any restart until then.
        pending = null;
        settling = true;
        if (current(owner)) {
          if (success) owner.started = true;
          else unavailable(owner);
        } else if (success) stop(owner.sensor);
        queueMicrotask(() => { settling = false; start(); });
      });
    } catch {
      if (pending?.owner === owner) pending = null;
      unavailable(owner);
    }
  }

  return {
    activate(session) {
      if (disposed) return;
      if (selected === session && lease) return;
      selected = session;
      failed = false;
      const stopped = release();
      if (!stopped) { failed = true; notify(session, 'unavailable'); return; }
      failed = !supported();
      notify(session, paused ? 'paused' : failed ? 'unavailable' : 'waiting');
      start();
    },
    pause(value) {
      if (disposed || paused === Boolean(value)) return;
      paused = Boolean(value);
      failed = false;
      if (paused) {
        const stopped = release();
        if (selected !== null) notify(selected, stopped ? 'paused' : 'unavailable');
      } else if (selected !== null) {
        failed = !supported();
        notify(selected, failed ? 'unavailable' : 'waiting');
        start();
      }
    },
    deactivate() {
      selected = null;
      failed = false;
      release();
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      selected = null;
      release();
    },
  };
}
