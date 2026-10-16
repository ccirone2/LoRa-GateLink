// The remaining controls: the key, identify, the ping card's margin, auto-ping, replay, history clear, and the board coming back after a
// cable drop (and being a different board).
import { test, expect, openConsole, connect, requests } from './helpers.js';

test('Generate makes a 32-hex key and Write sends exactly it', async ({ page }) => {
  await openConsole(page, [{ role: 'house', keySet: false }]);
  await connect(page);
  await expect(page.locator('#healthLine')).toHaveText(/^No link key/);
  await page.locator('#tabbtn-board').click();
  await page.locator('#btnKeyGen').click();
  const key = await page.locator('#keyInput').inputValue();
  expect(key).toMatch(/^[0-9a-f]{32}$/);
  await page.locator('#btnKeySet').click();
  await expect(page.locator('#toast')).toContainText('Key written and saved');
  expect((await requests(page)).find((r) => r.cmd === 'key.set').key).toBe(key);
  await expect(page.locator('#healthLine')).toHaveText(/^All good/); // at once, not on the next poll
  await expect(page.locator('#secKeySet')).toHaveText('yes');
});

test('a pasted key with separators is accepted; a short one is flagged and not sent', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#tabbtn-board').click();
  await page.locator('#keyInput').fill('00112233');
  await expect(page.locator('#keyInput')).toHaveAttribute('aria-invalid', 'true');
  await expect(page.locator('#keyHint')).toHaveText('8/32 characters');
  await page.locator('#btnKeySet').click();
  await expect(page.locator('#toast')).toContainText('exactly 32 hex characters');
  await page.locator('#keyInput').fill('00:11:22:33:44:55:66:77-88-99 aa bb cc dd ee ff');
  await expect(page.locator('#keyInput')).toHaveAttribute('aria-invalid', 'false');
  await page.locator('#btnKeySet').click(); // the board has a key: confirm accepted
  await expect(page.locator('#toast')).toContainText('Key written and saved');
  expect((await requests(page)).find((r) => r.cmd === 'key.set').key).toBe('00112233445566778899aabbccddeeff');
});

test('Identify, replay and history clear send their commands', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#btnIdentify').click();
  await expect(page.locator('#toast')).toHaveText('LED strobing for 6 s.');
  await page.locator('#tabbtn-board').click();
  await page.getByText('Developer', { exact: true }).click();
  await page.locator('#btnReplay').click();
  await expect(page.locator('#toast')).toContainText('Replay sent');
  await page.locator('#tabbtn-link').click();
  await page.locator('#btnHistClear').click();
  await expect(page.locator('#toast')).toHaveText('Link history cleared.');
  const cmds = (await requests(page)).map((r) => r.cmd);
  expect(cmds).toEqual(expect.arrayContaining(['identify', 'debug.replay', 'hist.clear', 'hist.get']));
});

test('each pong shows both directions’ margin, coloured by the survey’s thresholds', async ({ page }) => {
  // SF 9 (floor −12.5 dB); the house knows both noise floors (−118 here, −117 at the gate) from its status.
  await openConsole(page, [{ role: 'house', pongs: [{}, { snr: -1, rssi: -110, peer_snr: -4, peer_rssi: -112 },
    { peer_snr: null, peer_rssi: null }] }]);
  await connect(page);
  await page.locator('#tabbtn-link').click();
  const margin = page.locator('#pingMargin');
  await page.locator('#btnPing').click();
  await expect(margin).toHaveText('69.5 / 69.5 dB'); // strong: RSSI above the noise floor, not the saturated SNR
  await expect(margin.locator('.lvl-good')).toHaveCount(2);
  await page.locator('#btnPing').click();
  await expect(margin).toHaveText('11.5 / 8.5 dB');
  await expect(margin.locator('.lvl-fair')).toHaveText('11.5');
  await expect(margin.locator('.lvl-marginal')).toHaveText('8.5');
  await page.locator('#btnPing').click();
  await expect(margin).toHaveText('69.5 / — dB'); // the peer's levels missing (null here: undefined doesn't reach the fake board)
});

test('auto-ping pings every 3 s until switched off', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#tabbtn-link').click();
  await page.locator('#pingAuto').check();
  await expect.poll(async () => (await requests(page)).filter((r) => r.cmd === 'radio.ping').length, { timeout: 8000 })
    .toBeGreaterThanOrEqual(2);
  await page.locator('#pingAuto').uncheck();
  const n = (await requests(page)).filter((r) => r.cmd === 'radio.ping').length;
  await page.waitForTimeout(3500);
  expect((await requests(page)).filter((r) => r.cmd === 'radio.ping').length).toBe(n);
});

test('after the cable drops the page reconnects when the board comes back', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.evaluate(() => window.__fake.ports[0].replug(800));
  await expect(page.locator('#devline')).toHaveText('reconnecting…');
  await expect(page.locator('#devline')).toHaveText('MKR WAN 1310 · fw 0.13.8 · house', { timeout: 10000 });
  await page.locator('#tabbtn-log').click();
  await expect(page.locator('#logView')).toContainText('reconnected');
  await expect(page.locator('#logView')).not.toContainText('port close failed');
});

test('a different board coming back on the port is pointed out', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.evaluate(() => {
    const b = window.__fake.boards[0];
    b.fw = '0.14.0'; // as if another board had been plugged into the same port
    window.__fake.ports[0].replug(500);
  });
  await expect(page.locator('#toast')).toContainText('Reconnected to a different board', { timeout: 10000 });
});

test('disconnect waits for the streams, so the port really closes and opens again', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#btnDisconnect').click();
  await expect(page.locator('#devline')).toHaveText('not connected');
  expect(await page.evaluate(() => window.__fake.ports[0].isOpen)).toBe(false);
  await connect(page);
  await page.locator('#tabbtn-log').click();
  await expect(page.locator('#logView')).not.toContainText('port close failed');
});
