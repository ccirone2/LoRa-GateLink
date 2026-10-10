// The Log and Tools tabs: decoded log lines, ping, remote diagnostics and writes, relay tests, link history.
import { test, expect, openConsole, connect, requests, pushEvent } from './helpers.js';

test('log events are shown decoded, and the download keeps the board’s own form', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await pushEvent(page, { event: 'log', t: 65000, ev: 'gate_state', a: 3, b: 1 });
  await pushEvent(page, { event: 'log', t: 66000, ev: 'cmd_sent', a: 2, b: 77 });
  await pushEvent(page, { event: 'log', t: 67000, ev: 'cfg_remote', a: 16, b: 700 });
  await page.locator('#tabbtn-log').click();
  await expect(page.locator('#logView')).toContainText('[1m 5s] gate between (lora)');
  await expect(page.locator('#logView')).toContainText('CLOSE command sent (#77)');
  await expect(page.locator('#logView')).toContainText('remote write: pulse_ms = 700');
  const [download] = await Promise.all([page.waitForEvent('download'), page.locator('#btnLogSave').click()]);
  const text = Buffer.concat(await (await download.createReadStream()).toArray()).toString();
  expect(text).toContain('[1m 5s] gate_state a=3 b=1');
  expect(text).not.toContain('gate between (lora)');
});

test('the board log is fetched and decoded', async ({ page }) => {
  await openConsole(page, [{ role: 'gate', log: [{ t: 1000, ev: 'boot', a: 0x20, b: 2 }, { t: 1200, ev: 'pulse', a: 1, b: 500 }] }]);
  await connect(page, 'gate');
  await page.locator('#tabbtn-log').click();
  await page.locator('#btnLogGet').click();
  await expect(page.locator('#logView')).toContainText('booted (watchdog reset), role gate');
  await expect(page.locator('#logView')).toContainText('K1 pulsed for 500 ms');
});

test('ping shows round trip, both ends and the frequency error', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#btnPing').click();
  await expect(page.locator('#pingRtt')).toHaveText('142 ms');
  await expect(page.locator('#pingHere')).toHaveText('-61 dBm / 9.3 dB');
  await expect(page.locator('#pingPeer')).toHaveText('-60 dBm / 9.0 dB');
  await expect(page.locator('#pingFei')).toContainText('-1450 Hz');
});

test('remote diagnostics and a remote write report back', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#btnDiag').click();
  await expect(page.locator('#diagOut')).toContainText('fw 0.13.6 · up 1h 0m');
  await page.locator('#remParam').selectOption('pulse_ms');
  await page.locator('#remValue').fill('700');
  await page.locator('#btnRemSet').click();
  await expect(page.locator('#remResult')).toHaveText('Gate accepted and saved the value.');
  expect((await requests(page)).find((r) => r.cmd === 'remote.set')).toMatchObject({ name: 'pulse_ms', value: 700 });
});

test('a gate relay test asks first, then pulses', async ({ page }) => {
  await openConsole(page, [{ role: 'gate' }]);
  await connect(page, 'gate');
  await page.locator('#tabbtn-tools').click();
  await page.locator('#btnK2').click();
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('pulse the opener CLOSE input');
  await expect(page.locator('#toast')).toHaveText('K2 pulsed for 500 ms.');
  expect((await requests(page)).find((r) => r.cmd === 'relay.test')).toMatchObject({ k: 2, ms: 500 });
});

test('link history loads every page and draws the chart, tiles and table', async ({ page }) => {
  await openConsole(page, [{ role: 'house', histBuckets: 30 }]);
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#btnHistLoad').click();
  await expect(page.locator('#histBody')).toBeVisible();
  await expect(page.locator('#histTable tbody tr')).toHaveCount(30);
  await expect(page.locator('#histTiles .tile')).toHaveCount(4);
  await expect(page.locator('#histChart polyline.ln.here')).not.toHaveCount(0);
  const pages = (await requests(page)).filter((r) => r.cmd === 'hist.get');
  expect(pages.length).toBe(3); // 12 + 12 + 6
  const [download] = await Promise.all([page.waitForEvent('download'), page.locator('#btnHistCsv').click()]);
  const csv = Buffer.concat(await (await download.createReadStream()).toArray()).toString().trim().split('\n');
  expect(csv).toHaveLength(31);
  expect(csv[0]).toMatch(/^start,idx,tx,rx,/);
});
