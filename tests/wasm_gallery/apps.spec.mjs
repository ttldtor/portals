// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { test, expect } from './playwright.mjs';
import { activateControl } from './gallery_helpers.mjs';

test.use({ hasTouch: true });

async function withOrientation(page) {
  await page.route('https://telegram.org/js/**', route => route.fulfill({ contentType: 'application/javascript', body: `
    const events = new Map();
    window.probe = { starts: 0, stops: 0, params: [], captured: [],
      emit(name, value) { for (const fn of [...(events.get(name) || [])]) fn(value); },
      sample(beta, gamma) { Object.assign(Telegram.WebApp.DeviceOrientation, {beta, gamma}); this.emit('deviceOrientationChanged'); },
      capture() { this.captured = [...(events.get('deviceOrientationChanged') || [])]; },
      late(beta, gamma) { Object.assign(Telegram.WebApp.DeviceOrientation, {beta, gamma}); for (const fn of this.captured) fn(); },
      sensorListeners() { return [...events].filter(([name]) => name.startsWith('deviceOrientation')).reduce((n, [, list]) => n + list.size, 0); },
      listeners() { return [...events.values()].reduce((n, list) => n + list.size, 0); }
    };
    window.Telegram = { WebApp: { platform: 'android', isActive: true, viewportStableHeight: 760,
      isVersionAtLeast: () => true, ready() {}, expand() {},
      onEvent(name, fn) { if (!events.has(name)) events.set(name, new Set()); events.get(name).add(fn); },
      offEvent(name, fn) { events.get(name)?.delete(fn); },
      DeviceOrientation: { isStarted: false, beta: 0, gamma: 0,
        start(params, callback) { probe.starts++; probe.params.push(params); this.isStarted = true; callback?.(true); probe.emit('deviceOrientationStarted'); return this; },
        stop(callback) { probe.stops++; this.isStarted = false; callback?.(true); probe.emit('deviceOrientationStopped'); return this; }
      }
    } };
  ` }));
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
}

async function choose(page, name) {
  const button = page.getByRole('button', { name, exact: true });
  await expect(button).toBeVisible();
  await button.click();
  await expect(button).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('.xterm-rows')).toContainText(name === 'Level' ? 'Phone level' : 'Portals gallery');
}

test('switcher opens Level without SDK, reports missing sensor and returns to Gallery', async ({ page }) => {
  await page.route('https://telegram.org/js/**', route => route.fulfill({ contentType: 'application/javascript', body: '' }));
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  await choose(page, 'Level');
  await expect(page.locator('.xterm-rows')).toContainText('Sensor unavailable');
  await expect(page.locator('.xterm-helper-textarea')).toHaveAttribute('readonly', '');
  await choose(page, 'Gallery');
  await expect(page.locator('.xterm-helper-textarea')).not.toHaveAttribute('readonly', '');
  await page.locator('.xterm-helper-textarea').focus();
  await page.locator('.xterm-helper-textarea').pressSequentially(' switch');
  await expect(page.locator('.xterm-rows')).toContainText('Portals switch');
});

test('Shift-Tab stays inside Level and still reverses Gallery traversal after returning', async ({ page }) => {
  await withOrientation(page);
  await choose(page, 'Level');
  await page.evaluate(() => probe.sample(0, 0));
  await expect(page.locator('.xterm-rows')).toContainText(/X:\s*\+?0\.0/);
  await activateControl(page, 'Zero');
  const input = page.locator('.xterm-helper-textarea');
  await input.press('Shift+Tab');
  await expect(input).toBeFocused();
  await input.press('Enter');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  await choose(page, 'Gallery');
  await input.press('Shift+Tab');
  await expect(input).toBeFocused();
  await input.press('Tab');
  await input.pressSequentially(' reverse');
  await expect(page.locator('.xterm-rows')).toContainText('Portals reverse');
});

test('rapid application requests keep only the latest scene and page hide closes pending work', async ({ page }) => {
  await withOrientation(page);
  await page.evaluate(() => {
    const gallery = document.querySelector('[data-app="gallery"]');
    const level = document.querySelector('[data-app="level"]');
    level.click(); gallery.click(); level.click();
  });
  await expect(page.locator('.xterm-rows')).toContainText('Phone level');
  await expect.poll(() => page.evaluate(() => probe.starts)).toBe(1);
  await page.evaluate(() => {
    document.querySelector('[data-app="gallery"]').click();
    dispatchEvent(new PageTransitionEvent('pagehide'));
  });
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  expect(await page.evaluate(() => probe.listeners())).toBe(0);
  await expect(page.getByRole('button', { name: 'Gallery', exact: true })).toBeDisabled();
});

test('real WASM Level receives orientation, calibrates Zero, and stops sensor on leaving', async ({ page }, testInfo) => {
  await withOrientation(page);
  expect(await page.evaluate(() => probe.starts)).toBe(0);
  await choose(page, 'Level');
  await expect.poll(() => page.evaluate(() => probe.starts)).toBe(1);
  expect(await page.evaluate(() => probe.params[0])).toEqual({ refresh_rate: 50, need_absolute: false });
  await page.evaluate(() => probe.sample(Math.PI / 18, Math.PI / 9));
  await expect(page.locator('.xterm-rows')).toContainText(/X:\s*\+?20\.0/);
  await expect(page.locator('.xterm-rows')).toContainText(/Y:\s*\+?10\.0/);
  await expect(page.locator('.xterm-rows')).toContainText('●');
  await activateControl(page, 'Zero', true);
  await expect(page.locator('.xterm-rows')).toContainText(/X:\s*\+?0\.0/);
  await expect(page.locator('.xterm-rows')).toContainText(/Y:\s*\+?0\.0/);
  await page.screenshot({ path: testInfo.outputPath('level-zero.png') });
  await page.evaluate(() => probe.capture());
  await choose(page, 'Gallery');
  expect(await page.evaluate(() => probe.stops)).toBeGreaterThan(0);
  expect(await page.evaluate(() => probe.sensorListeners())).toBe(0);
  await page.evaluate(() => probe.late(1, 1));
  await expect(page.locator('.xterm-rows')).toContainText('Portals gallery');
  await choose(page, 'Level');
  await page.evaluate(() => probe.late(1, 1));
  await expect(page.locator('.xterm-rows')).toContainText('--');
  await page.evaluate(() => probe.sample(0, 0));
  await expect(page.locator('.xterm-rows')).toContainText(/X:\s*\+?0\.0/);
});

test('switching cancels Gallery animation; Quit leaves the application selector usable', async ({ page }) => {
  await withOrientation(page);
  await activateControl(page, 'Run something');
  await expect.poll(() => page.locator('.xterm-rows > div').nth(10).textContent(), { intervals: [30] }).toContain('s(O)mething');
  await choose(page, 'Level');
  await page.waitForTimeout(650);
  await expect(page.locator('.xterm-rows')).not.toContainText('someth');
  await activateControl(page, 'Quit');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  expect(await page.evaluate(() => probe.sensorListeners())).toBe(0);
  await choose(page, 'Gallery');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  await expect(page.locator('.xterm-rows > div').nth(10)).toContainText('(S)omething');
});

test('Level pauses sensors, ignores late events, resumes and disposes on page hide', async ({ page }) => {
  await withOrientation(page);
  await choose(page, 'Level');
  await page.evaluate(() => { probe.sample(0, 0); probe.capture(); Telegram.WebApp.isActive = false; probe.emit('deactivated'); });
  await expect.poll(() => page.evaluate(() => probe.sensorListeners())).toBe(0);
  await page.evaluate(() => probe.late(1, 1));
  const before = await page.locator('.xterm-rows').textContent();
  await page.waitForTimeout(200);
  expect(await page.locator('.xterm-rows').textContent()).toBe(before);
  await page.evaluate(() => { Telegram.WebApp.isActive = true; probe.emit('activated'); });
  await expect.poll(() => page.evaluate(() => probe.starts)).toBe(2);
  await page.evaluate(() => probe.sample(0, 0));
  await expect(page.locator('.xterm-rows')).toContainText(/X:\s*\+?0\.0/);
  await page.evaluate(() => dispatchEvent(new PageTransitionEvent('pagehide')));
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  expect(await page.evaluate(() => probe.listeners())).toBe(0);
});

test('Level remains visible on a phone and uses the current screen orientation', async ({ page }, testInfo) => {
  await page.setViewportSize({ width: 360, height: 740 });
  await withOrientation(page);
  await choose(page, 'Level');
  await page.evaluate(() => { Object.defineProperty(screen.orientation, 'angle', { value: 90, configurable: true }); probe.sample(0, Math.PI / 18); });
  await expect(page.locator('.xterm-rows')).toContainText(/X:\s*\+?0\.0/);
  await expect(page.locator('.xterm-rows')).toContainText(/Y:\s*-10\.0/);
  await expect(page.locator('#status')).toBeInViewport();
  await expect(page.locator('.xterm-rows > div').filter({ hasText: '[ Zero ]' })).toBeInViewport();
  await page.screenshot({ path: testInfo.outputPath('level-phone.png') });
});

test('the selected theme is shared across fresh Gallery and Level scenes', async ({ page }) => {
  await withOrientation(page);
  const title = text => page.locator('.xterm-rows > div').first().locator('span').filter({ hasText: text });
  await activateControl(page, 'Theme');
  await expect(title('Portals gallery')).toHaveCSS('background-color', 'rgb(0, 0, 170)');
  await choose(page, 'Level');
  await expect(title('Phone level')).toHaveCSS('background-color', 'rgb(0, 0, 170)');
  await activateControl(page, 'Theme', true);
  await expect(title('Phone level')).toHaveCSS('background-color', 'rgb(46, 52, 64)');
  await choose(page, 'Gallery');
  await expect(title('Portals gallery')).toHaveCSS('background-color', 'rgb(46, 52, 64)');
});
