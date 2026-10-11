// The Tools tab's site survey card: pings for the chosen time on a fake clock, judges both directions, pauses the ping
// card meanwhile, counts unanswered pings, stops on request and on disconnect, and copies a plain-text report.
import { test, expect, openConsole, connect, requests, fixture } from './helpers.js';

const pings = async (page) => (await requests(page)).filter((r) => r.cmd === 'radio.ping').length;

async function startSurvey(page, boards, minutes = '1') {
  await page.clock.install(); // runs on its own until a test moves it on with runFor
  await openConsole(page, boards);
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#surveyMins').selectOption(minutes);
  await page.locator('#btnSurvey').click();
  await expect(page.locator('#btnSurvey')).toHaveText('Stop survey');
}

test('a survey pings for the chosen time, one at a time, and judges both directions', async ({ page }) => {
  // House at SF9 (the fixture), noise floors -118 here and -117 at the gate: the house hears the gate with 38.5 dB in
  // hand (SNR 6.5 reads saturated, so RSSI − noise counts), the gate hears the house with 15.5 dB (SNR 3: SNR alone).
  await page.clock.install();
  await openConsole(page, [{ role: 'house', pongs: [{ rssi: -92, snr: 6.5, peer_rssi: -95, peer_snr: 3 }] }]);
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#pingAuto').check();
  await page.locator('#btnSurvey').click(); // 2 min, the default
  await expect(page.locator('#btnSurvey')).toHaveText('Stop survey');
  // The firmware answers only its latest ping: the ping card pauses, auto-ping included.
  await expect(page.locator('#pingAuto')).not.toBeChecked();
  await expect(page.locator('#pingAuto')).toBeDisabled();
  await expect(page.locator('#btnPing')).toBeDisabled();
  await expect(page.locator('#btnSurveyCopy')).toBeDisabled();
  const before = await pings(page);
  await page.clock.runFor(60000);
  await expect(page.locator('#surveyStep')).toContainText('of 2:00 · ');
  await expect(page.locator('#surveyVerdict')).toHaveText('Good so far');
  expect(await pings(page) - before).toBeLessThanOrEqual(31); // one every 2 s, auto-ping silent
  await page.clock.runFor(62000);
  await expect(page.locator('#btnSurvey')).toHaveText('Start survey');
  await expect(page.locator('#surveyStep')).toContainText('Done after 1:5');
  await expect(page.locator('#surveyStep')).toContainText('60 pings, 0 unanswered');
  await expect(page.locator('#surveyVerdict')).toHaveText('Good');
  await expect(page.locator('#surveyHeadline')).toHaveText(
    'the weaker direction (at the gate) keeps 15.5 dB above the SF9 limit in 90 % of pings (good from 15 dB, fair from 10 dB).');
  const rows = page.locator('#surveyTable tbody tr');
  await expect(rows.nth(0).locator('td')).toHaveText(['at the house', '60', '-92.0 / -92.0 dBm', '6.5 / 6.5 dB', '38.5 / 38.5 dB']);
  await expect(rows.nth(1).locator('td')).toHaveText(['at the gate', '60', '-95.0 / -95.0 dBm', '3.0 / 3.0 dB', '15.5 / 15.5 dB']);
  await expect(page.locator('#surveyAdvice li')).toHaveText(['Plenty of margin: nothing to change.']);
  // The ping card is back as it was, and saw the survey's pongs.
  await expect(page.locator('#pingAuto')).toBeChecked();
  await expect(page.locator('#btnPing')).toBeEnabled();
  await expect(page.locator('#pingHere')).toHaveText('-92 dBm / 6.5 dB');
  // Noise floors are re-read every 10 s, not before every ping.
  const statusReads = (await requests(page)).filter((r) => r.cmd === 'status').length;
  expect(statusReads).toBeLessThan(120);
});

test('unanswered pings count as lost; Stop ends it, and the report copies as text', async ({ page }) => {
  await startSurvey(page, [{ role: 'house', fw: '0.13.6', pongs: [{}, null, { rssi: '<b>x</b>', snr: 9 }] }]);
  await page.clock.runFor(30000); // 0 ok, 2 lost (given up at 10), 10 ok, 12 ok, 14 lost (22), 22 ok, 24 ok
  await page.locator('#btnSurvey').click();
  await expect(page.locator('#btnSurvey')).toHaveText('Start survey');
  await expect(page.locator('#surveyStep')).toContainText('Stopped after 0:');
  await expect(page.locator('#surveyVerdict')).toHaveText('Poor');
  await expect(page.locator('#surveyHeadline')).toContainText('of pings went unanswered (more than 5.0 %)');
  await expect(page.locator('#surveyAdvice')).toContainText('Frames are being lost');
  await expect(page.locator('#surveyAdvice')).toContainText('Raise tx_power (17 dBm on this board, at most 20) on both boards');
  await expect(page.locator('#surveyAdvice')).toContainText('Or raise sf (now 9) on BOTH boards');
  // A level that isn't a number is left out.
  await expect(page.locator('#surveyTable tbody tr').nth(0).locator('td').nth(2)).toHaveText('-61.0 / -61.0 dBm');

  await page.evaluate(() => {
    navigator.clipboard.writeText = async (t) => { window.__copied = t; };
  });
  await page.locator('#btnSurveyCopy').click();
  await expect(page.locator('#toast')).toHaveText('Survey report copied to the clipboard.');
  const text = await page.evaluate(() => window.__copied);
  expect(text).toContain('Board: house, firmware 0.13.6');
  expect(text).toContain('Radio: SF9, 500 kHz, tx_power 17 dBm on this board');
  expect(text).toMatch(/Ran 0:\d\d \(stopped\): \d+ pings, 2 unanswered/);
  expect(text).toMatch(/^at the gate +\d+ +-60\.0 \/ -60\.0 dBm/m);
  expect(text).toContain('- Frames are being lost');
});

test('an unanswered ping\'s late pong is not taken for the next ping\'s answer', async ({ page }) => {
  // Pings 1 and 4 go unanswered, and each one's pong (weak, -120 dBm) lands late, just before the next ping reaches
  // the board, which answers that one as usual.
  const late = { rssi: -120, snr: -5, peer_rssi: -121, peer_snr: -6 };
  await startSurvey(page, [{ role: 'house', pongs: [{}, null, { late }] }]);
  await page.clock.runFor(30000); // 0 ok, 2 lost (given up at 10), 10 ok, 12 ok, 14 lost (22), 22 ok, 24 ok
  await page.locator('#btnSurvey').click();
  await expect(page.locator('#surveyStep')).toContainText(', 2 unanswered');
  const rows = page.locator('#surveyTable tbody tr');
  await expect(rows.nth(0).locator('td').nth(2)).toHaveText('-61.0 / -61.0 dBm');
  await expect(rows.nth(1).locator('td').nth(2)).toHaveText('-60.0 / -60.0 dBm');
});

test('a role string from the board reaches the table as text', async ({ page }) => {
  // No firmware sends one like this; the page must still show it as text, never markup.
  await page.clock.install();
  await openConsole(page, [{ role: 'house', roleText: '<i>x</i>' }]);
  await page.locator('#btnConnect').click();
  await page.locator('#pickList .pick-item.ok .pick-main').click();
  await expect(page.locator('#devline')).toContainText('<i>x</i>');
  await page.locator('#tabbtn-tools').click();
  await page.locator('#btnSurvey').click();
  await expect(page.locator('#btnSurvey')).toHaveText('Stop survey');
  await page.clock.runFor(5000);
  await expect(page.locator('#surveyTable tbody tr').nth(0).locator('td').nth(0)).toHaveText('at the <i>x</i>');
  await expect(page.locator('#surveyTable i')).toHaveCount(0);
  await expect(page.locator('#surveyStep i')).toHaveCount(0);
});

test('a disconnect ends the survey and keeps what it found; another board clears it', async ({ page }) => {
  await startSurvey(page, [{ role: 'house' }, { role: 'gate' }]);
  await page.clock.runFor(10000);
  await page.evaluate(() => window.__fake.ports[0].unplug());
  await expect(page.locator('#surveyStep')).toContainText('Board disconnected after 0:1');
  await expect(page.locator('#surveyVerdict')).toHaveText('Good');
  await expect(page.locator('#btnSurvey')).toHaveText('Start survey');
  await expect(page.locator('#btnSurveyCopy')).toBeEnabled(); // the results can still be copied
  const n = await pings(page);
  await page.clock.runFor(10000);
  expect(await pings(page)).toBe(n);

  await connect(page, 'gate'); // Connect ends the auto-reconnect
  await page.locator('#tabbtn-tools').click();
  await expect(page.locator('#surveyBody')).toBeHidden();
  await expect(page.locator('#surveyStep')).toHaveText('');
  await expect(page.locator('#btnSurveyCopy')).toBeDisabled();
});

test('a survey needs the link up', async ({ page }) => {
  await openConsole(page, [{ role: 'gate', status: { link: { ...fixture.status_common.link, verified: false } } }]);
  await connect(page, 'gate');
  await page.locator('#tabbtn-tools').click();
  await page.locator('#btnSurvey').click();
  await expect(page.locator('#toast')).toContainText('Survey needs the link up');
  await expect(page.locator('#btnSurvey')).toHaveText('Start survey');
  expect(await pings(page)).toBe(0);
});
