// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { test, expect } from './playwright.mjs';

test('Shift-Tab keeps browser focus in the terminal and reverses the control traversal', async ({ page }) => {
  await page.route('https://telegram.org/js/**', route => route.fulfill({ contentType: 'application/javascript', body: '' }));
  await page.goto('./');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'running');
  const input = page.locator('.xterm-helper-textarea');
  await input.focus();
  await input.press('Shift+Tab');
  await expect(input).toBeFocused();
  // Starting at Name: reverse reaches Quit, forward returns to Name.
  await input.press('Tab');
  await input.pressSequentially(' reverse');
  await expect(page.locator('.xterm-rows')).toContainText('Portals reverse');
  await input.press('Tab');
  await input.press('Shift+Tab');
  await expect(input).toBeFocused();
  await input.pressSequentially(' again');
  await expect(page.locator('.xterm-rows')).toContainText('Portals reverse again');
  await input.press('F10');
  await expect(page.locator('#status')).toHaveAttribute('data-state', 'finished');
});
