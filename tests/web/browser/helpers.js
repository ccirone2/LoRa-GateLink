// Shared setup for the browser tests: load the page with fake boards (fake-serial.js) and connect.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { test as base, expect } from '@playwright/test';

const fixture = JSON.parse(readFileSync(new URL('../fixtures/firmware.json', import.meta.url), 'utf8'));
const fakePath = fileURLToPath(new URL('../fake-serial.js', import.meta.url));

// boards: [{ role, status, params, keySet, fw, log, granted, ... }] (see fake-serial.js). Dialogs are accepted
// unless options.dismissDialogs.
export async function openConsole(page, boards = [{ role: 'house' }], options = {}) {
  await page.addInitScript((c) => { window.__fakeSerialConfig = c; }, { fixture, boards, ...options });
  await page.addInitScript({ path: fakePath });
  page.dialogs = [];
  page.on('dialog', (d) => {
    page.dialogs.push(d.message());
    return options.dismissDialogs ? d.dismiss() : d.accept();
  });
  await page.goto(options.hash ? `/index.html#${options.hash}` : '/index.html');
}

// Connects to the board with that role through the picker (or straight away when only one is granted and the
// picker lists it).
export async function connect(page, role = 'house') {
  await page.locator('#btnConnect').click();
  const name = `LoRa GateLink – ${role[0].toUpperCase()}${role.slice(1)}`;
  const item = page.locator('#pickList .pick-item.ok', { has: page.getByText(name, { exact: true }) }).locator('.pick-main');
  await item.click();
  await expect(page.locator('#devline')).toContainText(`· ${role}`);
}

// Requests the page sent to board i (the fake records them).
export const requests = (page, i = 0) => page.evaluate((n) => window.__fake.boards[n].requests, i);

// Pushes an unsolicited line from board i, as the firmware would.
export const pushEvent = (page, ev, i = 0) => page.evaluate(([e, n]) => window.__fake.ports[n].push(e), [ev, i]);
export const pushLine = (page, text, i = 0) => page.evaluate(([t, n]) => window.__fake.ports[n].pushLine(t), [text, i]);

export { fixture, expect };

// Every test fails on an uncaught error in the page: a module that throws at call time, an import that isn't there.
export const test = base.extend({
  page: async ({ page }, use) => {
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    await use(page);
    expect(errors, 'uncaught errors in the page').toEqual([]);
  },
});
