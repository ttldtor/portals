// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { test, expect } from './playwright.mjs';

async function mockTelegram(page) {
  await page.route('https://telegram.org/js/**', route => route.fulfill({ contentType: 'application/javascript', body: `
    const events = new Map();
    window.probe = { ready: 0, expanded: 0, frames: 0, events,
      emit(name, value) { for (const fn of events.get(name) || []) fn(value); } };
    const raf = window.requestAnimationFrame.bind(window);
    window.requestAnimationFrame = callback => raf(time => { probe.frames++; callback(time); });
    window.Telegram = { WebApp: {
      platform: 'tdesktop', colorScheme: 'dark', isActive: true, viewportStableHeight: 760,
      themeParams: { bg_color: '#182838', text_color: '#eeeeee', hint_color: '#bbccee' },
      safeAreaInset: { top: 8 }, contentSafeAreaInset: { top: 12 },
      isVersionAtLeast: () => true,
      ready() { probe.ready++; }, expand() { probe.expanded++; },
      onEvent(name, fn) { if (!events.has(name)) events.set(name, new Set()); events.get(name).add(fn); },
      offEvent(name, fn) { events.get(name)?.delete(fn); }
    } };
  ` }));
}

test('Telegram host pauses, resumes, stays idle after Quit and removes callbacks on page hide', async ({ page }) => {
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await mockTelegram(page);
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  expect(await page.evaluate(() => [probe.ready, probe.expanded])).toEqual([1, 1]);
  await expect(page.locator('#environment')).toContainText('Telegram');
  await page.evaluate(() => { Telegram.WebApp.isActive = false; probe.emit('deactivated'); });
  await page.waitForTimeout(200);
  const stopped = await page.evaluate(() => probe.frames);
  await page.waitForTimeout(200);
  expect(await page.evaluate(() => probe.frames)).toBe(stopped);
  await page.evaluate(() => { Telegram.WebApp.isActive = true; probe.emit('activated'); });
  await expect.poll(() => page.evaluate(() => probe.frames)).toBeGreaterThan(stopped);
  const input = page.locator('.xterm-helper-textarea');
  await input.focus();
  await input.press('F10');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  await page.evaluate(() => { Telegram.WebApp.isActive = true; probe.emit('activated'); });
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  const idleFrames = await page.evaluate(() => probe.frames);
  await page.waitForTimeout(150);
  expect(await page.evaluate(() => probe.frames)).toBe(idleFrames);
  await page.evaluate(() => dispatchEvent(new PageTransitionEvent('pagehide')));
  expect(await page.evaluate(() => [...probe.events.values()].reduce((sum, callbacks) => sum + callbacks.size, 0))).toBe(0);
  expect(errors).toEqual([]);
});

test('Telegram layout fits a narrow viewport and applies stable height and theme updates', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await mockTelegram(page);
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  expect(await page.locator('body').evaluate(element => getComputedStyle(element).paddingTop)).toBe('36px');
  await page.evaluate(() => { Telegram.WebApp.viewportStableHeight = 640; probe.emit('viewportChanged', { isStateStable: false }); });
  expect(await page.locator('body').evaluate(element => getComputedStyle(element).height)).toBe('760px');
  await page.evaluate(() => { probe.emit('viewportChanged', { isStateStable: true }); Telegram.WebApp.themeParams.bg_color = '#f0f1f2'; Telegram.WebApp.themeParams.text_color = '#17202a'; Telegram.WebApp.colorScheme = 'light'; probe.emit('themeChanged'); });
  await expect(page.locator('body')).toHaveCSS('height', '640px');
  await expect(page.locator('html')).toHaveCSS('background-color', 'rgb(240, 241, 242)');
  await page.screenshot({ path: testInfo.outputPath('telegram-narrow-simulated.png') });
});

test('unavailable Telegram SDK still permits the standalone gallery', async ({ page }) => {
  await page.route('https://telegram.org/js/**', route => route.abort());
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  await expect(page.locator('html')).toHaveAttribute('data-host', 'browser');
});

test('keyboard viewport keeps all gallery controls inside the visible height', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 390, height: 844 });
  await mockTelegram(page);
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  await page.evaluate(() => {
    Object.defineProperty(visualViewport, 'height', { value: 350, configurable: true });
    visualViewport.dispatchEvent(new Event('resize'));
  });
  await expect(page.locator('body')).toHaveCSS('height', '350px');
  await expect(page.locator('.xterm-rows')).toContainText('Name :');
  await expect(page.locator('.xterm-rows')).toContainText('(S)omething');
  await expect.poll(async () => {
    const box = await page.locator('.xterm-rows > div').nth(10).boundingBox();
    return box.y + box.height;
  }).toBeLessThan(350);
  const status = await page.locator('#status').boundingBox();
  expect(status.y + status.height).toBeLessThanOrEqual(350);
  await page.screenshot({ path: testInfo.outputPath('keyboard-viewport-simulated.png') });
  await page.evaluate(() => { delete visualViewport.height; visualViewport.dispatchEvent(new Event('resize')); });
  await expect(page.locator('body')).toHaveCSS('height', '760px');
});

for (const asset of ['portals-gallery.js', 'portals-gallery.wasm']) {
  test(`closing during ${asset} loading cannot start a late session`, async ({ page }) => {
    await mockTelegram(page);
    let release;
    let observed;
    const requested = new Promise(resolve => { observed = resolve; });
    const gate = new Promise(resolve => { release = resolve; });
    await page.route(`**/${asset}*`, async route => {
      observed();
      await gate;
      await route.continue();
    });
    try {
      await page.goto('./', { waitUntil: 'commit' });
      await requested;
      await page.evaluate(() => dispatchEvent(new PageTransitionEvent('pagehide')));
      await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
      release();
      await page.evaluate(async () => { await import(document.querySelector('script[type="module"]').src); });
      expect(await page.evaluate(() => probe.ready)).toBe(0);
      expect(await page.evaluate(() => [...probe.events.values()].reduce((sum, callbacks) => sum + callbacks.size, 0))).toBe(0);
      await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
    } finally {
      release();
    }
  });
}
