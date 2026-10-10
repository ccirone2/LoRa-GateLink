// Connecting, the board picker, reconnecting after a reboot, and lines the page can't handle.
import { test, expect, openConsole, connect, requests, pushLine } from './helpers.js';

test('picker lists both boards by role and connects to the one chosen', async ({ page }) => {
  await openConsole(page, [{ role: 'gate' }, { role: 'house' }]);
  await page.locator('#btnConnect').click();
  const items = page.locator('#pickList .pick-item.ok .pick-main b');
  await expect(items).toHaveText(['LoRa GateLink – House', 'LoRa GateLink – Gate']); // house first, whatever the order
  await page.locator('#pickList .pick-item.ok', { has: page.getByText('LoRa GateLink – Gate', { exact: true }) })
    .locator('.pick-main').click();
  await expect(page.locator('#devline')).toHaveText('MKR WAN 1310 · fw 0.13.8 · gate');
  await expect(page.locator('#btnDisconnect')).toBeVisible();
  await expect(page.locator('#btnConnect')).toBeHidden();
  // The page reads info, config and status on connect.
  const cmds = (await requests(page, 0)).map((r) => r.cmd);
  expect(cmds).toEqual(expect.arrayContaining(['info', 'config.get', 'status']));
});

test('with no board granted yet, Connect goes straight to the browser chooser', async ({ page }) => {
  await openConsole(page, [{ role: 'house', granted: false }]);
  await page.locator('#btnConnect').click();
  await expect(page.locator('#devline')).toContainText('house');
});

test('disconnect releases the board and greys out the controls', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#btnDisconnect').click();
  await expect(page.locator('#devline')).toHaveText('not connected');
  await expect(page.locator('body')).toHaveClass(/offline/);
  await expect(page.locator('#btnCfgApply')).toBeDisabled();
  expect(await page.evaluate(() => window.__fake.ports[0].isOpen)).toBe(false);
});

test('reboot: the page reconnects to the board when it comes back', async ({ page }) => {
  await openConsole(page, [{ role: 'house', rebootMs: 400 }]);
  await connect(page);
  await page.locator('#tabbtn-config').click();
  await page.locator('#btnReboot').click();
  await expect(page.locator('#devline')).toHaveText(/reconnecting|not connected|MKR/);
  await expect(page.locator('#devline')).toHaveText('MKR WAN 1310 · fw 0.13.8 · house', { timeout: 10000 });
  await page.locator('#tabbtn-log').click();
  await expect(page.locator('#logView')).toContainText('reconnected');
});

test('a line that breaks the page is logged and skipped, and the session carries on', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await pushLine(page, '{"event":"status","status":null}'); // renderStatus throws on it
  await pushLine(page, 'not json at all');
  await pushLine(page, '{"event":"log","t":5000,"ev":"link_up","a":0,"b":0}');
  await page.locator('#tabbtn-log').click();
  await expect(page.locator('#logView')).toContainText('couldn\'t handle a line from the board');
  await expect(page.locator('#logView')).toContainText('link up');
  await expect(page.locator('#devline')).toContainText('house'); // still connected
});

test('a browser without Web Serial shows the banner and keeps Install usable', async ({ page }) => {
  await openConsole(page, [], { noSerial: true, hash: 'install' });
  await expect(page.locator('#unsupported')).toBeVisible();
  await expect(page.locator('#btnConnect')).toBeDisabled();
  await expect(page.locator('#wiringTable')).toContainText('IN1 (A1)');
});

test('opened from a file the page says how to open it, instead of sitting there dead', async ({ page }) => {
  await page.goto(new URL('../../../web/index.html', import.meta.url).href);
  await expect(page.locator('#unsupported')).toBeVisible();
  await expect(page.locator('#unsupportedMsg')).toContainText('can’t run from a file');
});
