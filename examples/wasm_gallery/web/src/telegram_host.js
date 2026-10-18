// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
export function createTelegramHost(onActivityChange, { window = globalThis.window, document = globalThis.document } = {}) {
  const app = window?.Telegram?.WebApp;
  const telegram = Boolean(app?.platform && app.platform !== 'unknown');
  const platform = telegram ? app.platform : 'browser';
  const root = document.documentElement;
  const listeners = [];
  let disposed = false;
  let suspended;
  root.dataset.host = telegram ? 'telegram' : 'browser';

  function updateActivity() {
    const next = Boolean(document.hidden || (telegram && app.isActive === false));
    if (next !== suspended) {
      suspended = next;
      onActivityChange(next);
    }
  }

  function updateVisibleHeight() {
    if (disposed) return;
    const height = window.visualViewport?.height ?? window.innerHeight;
    if (Number.isFinite(height) && height > 0) root.style.setProperty('--poc-visible-height', `${height}px`);
  }

  function updateTheme() {
    const colors = { background: 'bg_color', text: 'text_color', hint: 'hint_color', border: 'secondary_bg_color' };
    for (const [name, key] of Object.entries(colors)) {
      const value = app.themeParams?.[key];
      if (typeof value === 'string' && /^#[\da-f]{6}$/i.test(value)) {
        root.style.setProperty(`--poc-${name}`, value);
      } else {
        root.style.removeProperty(`--poc-${name}`);
      }
    }
    if (app.colorScheme === 'light' || app.colorScheme === 'dark') root.style.colorScheme = app.colorScheme;
  }

  function updateLayout(event) {
    if (event?.isStateStable === false) return;
    const height = app.viewportStableHeight;
    if (Number.isFinite(height) && height > 0) root.style.setProperty('--poc-viewport-height', `${height}px`);
    for (const [name, inset] of [['safe', app.safeAreaInset], ['content', app.contentSafeAreaInset]]) {
      for (const side of ['top', 'bottom', 'left', 'right']) {
        const value = inset?.[side];
        const pixels = Number.isFinite(value) && value >= 0 ? value : 0;
        root.style.setProperty(`--poc-${name}-${side}`, `${pixels}px`);
      }
    }
  }

  function observe(name, callback, sdk = true) {
    const guarded = (event) => { if (!disposed) callback(event); };
    if (sdk) {
      app.onEvent(name, guarded);
      listeners.push(() => app.offEvent(name, guarded));
    } else {
      document.addEventListener(name, guarded);
      listeners.push(() => document.removeEventListener(name, guarded));
    }
  }

  observe('visibilitychange', updateActivity, false);
  for (const target of [window, window.visualViewport]) {
    if (!target?.addEventListener) continue;
    target.addEventListener('resize', updateVisibleHeight);
    listeners.push(() => target.removeEventListener('resize', updateVisibleHeight));
  }
  updateVisibleHeight();
  if (telegram) {
    updateTheme();
    updateLayout();
    observe('themeChanged', updateTheme);
    observe('viewportChanged', updateLayout);
    if (app.isVersionAtLeast?.('8.0')) {
      observe('safeAreaChanged', updateLayout);
      observe('contentSafeAreaChanged', updateLayout);
      observe('activated', updateActivity);
      observe('deactivated', updateActivity);
    }
  }
  updateActivity();

  return {
    telegram,
    platform,
    ready() {
      if (disposed || !telegram) return;
      app.ready();
      app.expand();
      updateLayout();
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      for (const remove of listeners) remove();
    },
  };
}
