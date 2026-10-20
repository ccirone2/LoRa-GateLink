// The Log, Link and Install tabs: decoded log lines, ping, remote diagnostics and writes, relay tests, link history.
import { test, expect, openConsole, connect, requests, pushEvent, cspViolations } from './helpers.js';

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

test('older events come from the flash journal, a record at a time, with their boot and coalesced counts', async ({ page }) => {
  await openConsole(page, [{ role: 'gate', boot: 9, journal: [
    [{ t: 1000, ev: 'boot', a: 0x20, b: 2, boot: 7 }],
    [{ t: 5000, ev: 'pulse', a: 1, b: 500, boot: 8 }, { t: 6000, ev: 'replay', a: 3, b: 90, n: 40, boot: 8 }],
  ] }]);
  await connect(page, 'gate');
  await page.locator('#tabbtn-log').click();
  await page.locator('#btnLogGet').click();
  await expect(page.locator('#logView')).toContainText('boot 9');
  await page.locator('#btnLogOlder').click();
  await expect(page.locator('#logView')).toContainText('journal record 1 (2 entries)');
  await expect(page.locator('#logView')).toContainText('[boot 8 5s] K1 pulsed for 500 ms');
  await expect(page.locator('#logView')).toContainText('(×40)');
  await page.locator('#btnLogOlder').click();
  await expect(page.locator('#logView')).toContainText('[boot 7 1s] booted (watchdog reset), role gate');
  await page.locator('#btnLogOlder').click();
  await expect(page.locator('#logView')).toContainText('no older entries');
  expect((await requests(page)).filter((r) => r.cmd === 'log.get').map((r) => r.rec ?? 0)).toEqual([0, 1, 2, 3]);
});

test('ping shows round trip, both ends and the frequency error; another board clears them', async ({ page }) => {
  await openConsole(page, [{ role: 'house' }, { role: 'gate' }]);
  await connect(page);
  await page.locator('#tabbtn-link').click();
  await page.locator('#btnPing').click();
  await expect(page.locator('#pingRtt')).toHaveText('142 ms');
  await expect(page.locator('#pingHere')).toHaveText('-61 dBm / 9.3 dB');
  await expect(page.locator('#pingPeer')).toHaveText('-60 dBm / 9.0 dB');
  await expect(page.locator('#pingFei')).toContainText('-1450 Hz');
  await page.locator('#btnDisconnect').click();
  await connect(page, 'gate');
  for (const id of ['#pingRtt', '#pingHere', '#pingPeer', '#pingFei']) await expect(page.locator(id)).toHaveText('—');
});

test('remote diagnostics and a remote write report back', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#tabbtn-link').click();
  await page.locator('#btnDiag').click();
  await expect(page.locator('#diagOut')).toContainText('fw 0.13.8 · up 1h 0m');
  await page.locator('#remParam').selectOption('pulse_ms');
  await page.locator('#remValue').fill('700');
  await page.locator('#btnRemSet').click();
  await expect(page.locator('#remResult')).toHaveText('Gate accepted and saved the value.');
  expect((await requests(page)).find((r) => r.cmd === 'remote.set')).toMatchObject({ name: 'pulse_ms', value: 700 });
});

test('a gate relay test asks first, then pulses', async ({ page }) => {
  await openConsole(page, [{ role: 'gate' }]);
  await connect(page, 'gate');
  await page.locator('#tabbtn-install').click();
  // Next to the gate's wiring, with its inputs as the board reads them.
  await expect(page.locator('[data-wiring=gate]')).toHaveClass(/active/);
  await expect(page.locator('#checkRole')).toHaveText('the connected gate board');
  await expect(page.locator('#ioCheck')).toContainText('IN1 · Open limit');
  await expect(page.locator('#btnK2')).toHaveText('Pulse K2 (CLOSE)');
  await page.locator('#btnK2').click();
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('pulse the opener CLOSE input');
  await expect(page.locator('#toast')).toHaveText('K2 pulsed for 500 ms.');
  expect((await requests(page)).find((r) => r.cmd === 'relay.test')).toMatchObject({ k: 2, ms: 500 });
});

test('link history loads every page and draws the chart, tiles and table', async ({ page }) => {
  await openConsole(page, [{ role: 'house', histBuckets: 30 }]);
  await connect(page);
  await page.locator('#tabbtn-link').click();
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
  await expect(page.locator('#histChart line.reset')).toHaveCount(0);
  await expect(page.locator('#histNote')).toContainText('kept across resets');
  expect(await cspViolations(page)).toEqual([]); // the page runs whole under its own policy
});

test('link history takes only numbers from the board: markup in a field is never put in the page', async ({ page }) => {
  const evil = '<img src=x onerror="window.__xss=1">';
  await openConsole(page, [{ role: 'house', histBuckets: 3, histOverride: { tx: evil, rssi_avg: evil, boot: evil } }]);
  await connect(page);
  await page.locator('#tabbtn-link').click();
  await page.locator('#btnHistLoad').click();
  await expect(page.locator('#histTable tbody tr')).toHaveCount(3);
  await expect(page.locator('#histBody img')).toHaveCount(0);
  await expect(page.locator('#histTable')).not.toContainText('onerror');
  expect(await page.evaluate(() => window.__xss)).toBeUndefined();
});

test('link history marks where the board reset, and the times before it as earlier', async ({ page }) => {
  await openConsole(page, [{ role: 'gate', histBuckets: 20, histResetAt: 15 }]);
  await connect(page, 'gate');
  await page.locator('#tabbtn-link').click();
  await page.locator('#btnHistLoad').click();
  await expect(page.locator('#histChart line.reset')).toHaveCount(1);
  await expect(page.locator('#histLegend')).toContainText('board reset');
  await expect(page.locator('#histNote')).toContainText('from before');
  // The table lists the newest first: buckets 19..15 after the reset, then 14..0 before it.
  const starts = await page.locator('#histTable tbody tr td:first-child').allTextContents();
  expect(starts.slice(0, 5).every((s) => !s.startsWith('before'))).toBe(true);
  expect(starts.slice(5).every((s) => s.startsWith('before '))).toBe(true);
  await expect(page.locator('#histTable tbody tr.reset')).toHaveCount(1);
  await expect(page.locator('#histTable tbody tr').nth(4)).toHaveClass(/reset/); // bucket 15: the line under it
});
