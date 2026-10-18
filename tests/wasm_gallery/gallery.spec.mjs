// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
import { test, expect } from './playwright.mjs';

test.beforeEach(async ({ page }) => {
  if (!process.env.PORTALS_PAGES_LIVE) {
    await page.route('https://telegram.org/js/**', route => route.fulfill({ contentType: 'application/javascript', body: '' }));
  }
});

test('real gallery accepts text, changes theme, resizes and quits', async ({ page }, testInfo) => {
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  const screen = page.locator('.xterm-accessibility-tree');
  await expect(screen).toContainText('Portals gallery');
  await page.screenshot({ path: testInfo.outputPath('gallery-initial.png') });
  const input = page.locator('.xterm-helper-textarea');
  await input.focus();
  await expect(page.locator('.xterm-cursor')).toHaveCount(0);
  await input.pressSequentially(' WASM');
  await input.evaluate(element => {
    const data = new DataTransfer();
    data.setData('text/plain', ' Привет');
    element.dispatchEvent(new ClipboardEvent('paste', { clipboardData: data, bubbles: true, cancelable: true }));
  });
  await expect(screen).toContainText('WASM Привет');
  await input.press('Enter');
  await expect(screen).toContainText('WASM Привет');
  const beforeTheme = await page.locator('#terminal').screenshot();
  await input.press('F2');
  await expect.poll(async () => (await page.locator('#terminal').screenshot()).equals(beforeTheme)).toBe(false);
  const beforeResize = await page.locator('.xterm-screen').boundingBox();
  await page.setViewportSize({ width: 800, height: 650 });
  await expect.poll(async () => (await page.locator('.xterm-screen').boundingBox()).width).toBeLessThan(beforeResize.width);
  await expect(page.locator('.xterm-rows')).toContainText('Portals gallery');
  await expect(screen).toContainText('Portals gallery');
  // xterm schedules its DOM paint separately from buffer/accessibility updates.
  await expect(page.locator('.xterm-cursor')).toHaveCount(0);
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  await page.screenshot({ path: testInfo.outputPath('gallery-interactive.png') });
  await input.press('Tab');
  await input.press('Space');
  await expect(screen).toContainText('[x] Alerts');
  await input.press('Tab');
  await input.press('Enter');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  await input.press('F10');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  await expect(screen).toContainText('Portals gallery');
  await page.setViewportSize({ width: 900, height: 700 });
  await input.press('Control+c');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  expect(errors).toEqual([]);
});

test('missing WASM is visible outside the terminal', async ({ page }) => {
  await page.route('**/portals-gallery.wasm*', route => route.abort());
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'error');
  await expect(page.locator('#status')).toContainText('Error:');
});

test('invalid WASM after fetch is visible', async ({ page }) => {
  await page.route('**/portals-gallery.wasm*', route => route.fulfill({
    contentType: 'application/wasm', body: Buffer.from([0, 1, 2, 3]) }));
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'error');
});

test('runtime input overflow reports an error and stops the host', async ({ page }) => {
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  const input = page.locator('.xterm-helper-textarea');
  await input.focus();
  await input.evaluate(element => { const data = new DataTransfer(); data.setData('text/plain', 'x'.repeat(5000)); element.dispatchEvent(new ClipboardEvent('paste', { clipboardData: data, bubbles: true, cancelable: true })); });
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'error');
  await expect(page.locator('#status')).toContainText('queue limit');
});

for (const key of ['F10', 'Control+c']) {
  test(`gallery quits with ${key} when Chromium delivers the key`, async ({ page }) => {
    await page.goto('./');
    await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
    const input = page.locator('.xterm-helper-textarea');
    await input.focus();
    await input.press(key);
    await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  });
}
