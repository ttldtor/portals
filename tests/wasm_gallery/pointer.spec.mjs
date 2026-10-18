// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { test, expect } from './playwright.mjs';
import { activateControl, controlPoint } from './gallery_helpers.mjs';

test.use({ hasTouch: true, viewport: { width: 390, height: 844 } });
test.beforeEach(async ({ page }) => {
  await page.route('https://telegram.org/js/**', route => route.fulfill({
    contentType: 'application/javascript', body: `
      window.Telegram = { WebApp: { platform: 'android', isActive: true,
        viewportStableHeight: 844, safeAreaInset: { top: 8 }, contentSafeAreaInset: { top: 12 },
        isVersionAtLeast: () => true, ready() {}, expand() {}, onEvent() {}, offEvent() {}
      } };
    `
  }));
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  await expect(page.locator('.xterm-rows')).toContainText('Portals gallery');
  await expect(page.locator('#environment')).toContainText('Telegram');
});

for (const method of ['mouse', 'touchscreen']) {
  test(`${method} activates real gallery controls once and focuses text input`, async ({ page }, testInfo) => {
    const activate = text => activateControl(page, text, method === 'touchscreen');
    const screen = page.locator('.xterm-accessibility-tree');
    await activate('Alerts');
    await expect(screen).toContainText('[x] Alerts');
    // Wait past compatibility mouse dispatch: one tap must not toggle twice.
    await page.waitForTimeout(400);
    await expect(screen).toContainText('[x] Alerts');
    const editor = await controlPoint(page, 'Portals', 'Name :');
    if (method === 'mouse') await page.mouse.click(editor.x, editor.y);
    else await page.touchscreen.tap(editor.x, editor.y);
    await page.locator('.xterm-helper-textarea').pressSequentially(' pointer');
    await expect(screen).toContainText('Portals pointer');
    const beforeResize = await page.locator('.xterm-screen').boundingBox();
    await page.setViewportSize({ width: 480, height: 700 });
    await expect.poll(async () => (await page.locator('.xterm-screen').boundingBox()).width).toBeGreaterThan(beforeResize.width);
    await activate('Alerts');
    await expect(screen).toContainText('[ ] Alerts');
    await activate('Run something');
    // Screen-reader rows are throttled to 1s; observe the actual 300ms animation frame.
    await expect.poll(() => page.locator('.xterm-rows').textContent(), { intervals: [30] }).toContain('s(O)mething');
    await expect(page.locator('.xterm-cursor')).toHaveCount(0);
    await page.screenshot({ path: testInfo.outputPath(`${method}-gallery.png`) });
    await activate('Quit');
    await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
    await activate('Alerts');
    await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
  });
}

test('touch swipe and cancellation do not activate a checkbox', async ({ page, context }) => {
  const client = await context.newCDPSession(page);
  const { x, y } = await controlPoint(page, 'Alerts');
  await client.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: x + 50, y }] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y, id: 1 }] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y, id: 1 }, { x: 5, y, id: 2 }] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchCancel', touchPoints: [] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y, id: 1 }] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y, id: 1 }, { x: x + 30, y, id: 2 }] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await client.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x, y }] });
  await page.waitForTimeout(600);
  await client.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await page.waitForTimeout(100);
  await expect(page.locator('.xterm-accessibility-tree')).toContainText('[ ] Alerts');
  await page.touchscreen.tap(x, y);
  await expect(page.locator('.xterm-accessibility-tree')).toContainText('[x] Alerts');
});
