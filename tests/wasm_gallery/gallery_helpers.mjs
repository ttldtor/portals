// Copyright (c) 2026 ttldtor. SPDX-License-Identifier: BSL-1.0
import { expect } from './playwright.mjs';

export function galleryRow(page, text) {
  return page.locator('.xterm-rows > div').filter({ hasText: text }).first();
}

export async function controlPoint(page, text, rowText = text) {
  const row = galleryRow(page, rowText);
  await expect(row).toBeVisible();
  return row.evaluate((element, needle) => {
    let offset = element.textContent.indexOf(needle);
    const nodes = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
    for (let node = nodes.nextNode(); node; node = nodes.nextNode()) {
      if (offset >= node.length) { offset -= node.length; continue; }
      const range = document.createRange();
      range.setStart(node, offset);
      range.setEnd(node, offset + 1);
      const rect = range.getBoundingClientRect();
      return { x: rect.x + rect.width / 2, y: rect.y + rect.height / 2 };
    }
    throw new Error(`No rendered cell for ${needle}`);
  }, text);
}

export async function activateControl(page, text, touch = false) {
  const { x, y } = await controlPoint(page, text);
  if (touch) await page.touchscreen.tap(x, y);
  else await page.mouse.click(x, y);
}
