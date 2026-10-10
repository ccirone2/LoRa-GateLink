// The Status tab: gate state, link loss, the house's view of the gate, the tab title.
import { test, expect, openConsole, connect, pushEvent, fixture } from './helpers.js';

const houseStatus = (over = {}) => ({ ...fixture.status_common, role: 'house', ...fixture.status_house, ...over });

test('house: gate state, link and controller fields', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await expect(page.locator('#gateState')).toHaveText('closed');
  await expect(page.locator('#gateState')).toHaveClass(/closed/);
  await expect(page.locator('#lnkUp')).toHaveText('up');
  await expect(page.locator('#lnkVerified')).toHaveText('yes');
  await expect(page.locator('#hArmed')).toHaveText('yes');
  await expect(page.locator('#hLimits')).toHaveText('open ○  closed ●');
  await expect(page.locator('#hCtrl')).toHaveText('off');
  await expect(page.locator('#hCtrlPower')).toHaveText('ON');
  await expect(page).toHaveTitle('Closed · House · GateLink');
});

test('house: an unpowered controller shows its edges are ignored', async ({ page }) => {
  await openConsole(page, [{ role: 'house', status: { ctrl: true, ctrl_power: false } }]);
  await connect(page);
  await expect(page.locator('#hCtrl')).toHaveText('ON');
  await expect(page.locator('#hCtrlPower')).toHaveText('off · edges ignored');
});

test('house: a STATUS event updates the page at once', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await pushEvent(page, { event: 'status', status: houseStatus({ gate: 'between', cause: 'lora', target: 'open' }) });
  await expect(page.locator('#gateState')).toHaveText('between');
  await expect(page.locator('#gateCause')).toHaveText('lora');
  await expect(page.locator('#gateTarget')).toHaveText('open');
});

test('house: with the link down the last gate state shows as stale, not live', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await pushEvent(page, { event: 'status', status: houseStatus({ link_up: false }) });
  await expect(page.locator('#gateState')).toHaveClass(/stale/);
  await expect(page.locator('#gateStale')).toBeVisible();
  await expect(page.locator('#lnkUp')).toHaveText('down');
  await expect(page).toHaveTitle(/^Link lost/);
});

test('gate: no_power reads as words, with AC off and the first report waiting', async ({ page }) => {
  // As the firmware can report it: no limit reading and IN3 (AC) off.
  const io = { in1: false, in2: false, in3: false, in4: false, k1: false, k2: false };
  await openConsole(page, [{ role: 'gate', status: { gate: 'no_power', ac_power: false, settling: true, io } }]);
  await connect(page, 'gate');
  await expect(page.locator('#gateState')).toHaveText('no power');
  await expect(page.locator('#ioList .kv', { hasText: 'IN3 · AC power' }).locator('.pill')).toHaveText('off');
  await expect(page.locator('#ioList .kv', { hasText: 'IN2 · Closed limit' }).locator('.pill')).toHaveText('off');
  await expect(page.locator('#bSettling')).toHaveText('yes · first report waits');
});

test('health: ok with the fault output off, then problems and D5 as the board reports them', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await expect(page.locator('#bHealth')).toHaveText('ok · D5 off');
  // A problem inside fault_hold_s: listed, D5 still high.
  await pushEvent(page, { event: 'status', status: houseStatus({ fault_out: true, health: ['link'] }) });
  await expect(page.locator('#bHealth')).toHaveText('link down · D5 high');
  await expect(page.locator('#bHealth .bad')).toHaveText('link down');
  await pushEvent(page, { event: 'status', status: houseStatus({ fault_out: false, health: ['ac', 'no_power'] }) });
  await expect(page.locator('#bHealth')).toHaveText('no AC power at the gate, gate has no power · D5 low');
  await pushEvent(page, { event: 'status', status: houseStatus({ fault_out: true, health: [] }) });
  await expect(page.locator('#bHealth')).toHaveText('ok · D5 high');
});

test('health: fault_out turned on shows on the next status poll', async ({ page }) => {
  await openConsole(page, [{ role: 'gate' }]);
  await connect(page, 'gate');
  await expect(page.locator('#bHealth')).toHaveText('ok · D5 off');
  await page.evaluate(() => { window.__fake.boards[0].params.fault_out = 1; });
  await expect(page.locator('#bHealth')).toHaveText('ok · D5 high', { timeout: 5000 });
});

test('a status from older firmware without link or io fields still renders', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await pushEvent(page, { event: 'status', status: { role: 'house', fw: '0.3.0', gate: 'open', link_up: true } });
  await expect(page.locator('#gateState')).toHaveText('open');
  await expect(page.locator('#lnkCrc')).toHaveText('—');
  await expect(page.locator('#bHealth')).toHaveText('—');
});
