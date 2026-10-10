// The Install tab works without a board, per role, and fits a phone.
import { test, expect } from '@playwright/test';
import { openConsole } from './helpers.js';

test('wiring per board, from the WIRING table', async ({ page }) => {
  await openConsole(page, [], { hash: 'install' });
  await page.locator('[data-wiring="gate"]').click();
  await expect(page.locator('#wiringTable')).toContainText('OUT2 · closed limit');
  await expect(page.locator('#wiringSvg')).toHaveAttribute('aria-label', /Gate board field wiring/);
  await page.locator('[data-wiring="house"]').click();
  await expect(page.locator('#wiringTable')).toContainText('Controller switch input');
  await expect(page.locator('#wiringNotes li')).not.toHaveCount(0);
});

test('a phone-width window gets the compact diagram without horizontal scroll', async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 800 });
  await openConsole(page, [], { hash: 'install' });
  await expect(page.locator('#wiringSvg')).toHaveAttribute('viewBox', /^0 0 360 /);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
  expect(overflow).toBeLessThanOrEqual(0);
});
