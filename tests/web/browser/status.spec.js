// The Status tab: gate state, link loss, the house's view of the gate, the tab title.
import { test, expect } from '@playwright/test';
import { openConsole, connect, pushEvent, fixture } from './helpers.js';

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
  await expect(page).toHaveTitle('Closed · House · GateLink');
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

test('gate: no_power reads as words, AC and settling show', async ({ page }) => {
  await openConsole(page, [{ role: 'gate', status: { gate: 'no_power', ac_power: false, settling: true } }]);
  await connect(page, 'gate');
  await expect(page.locator('#gateState')).toHaveText('no power');
  await expect(page.locator('#bSettling')).toHaveText('yes · first report waits');
});

test('a status from older firmware without link or io fields still renders', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await pushEvent(page, { event: 'status', status: { role: 'house', fw: '0.3.0', gate: 'open', link_up: true } });
  await expect(page.locator('#gateState')).toHaveText('open');
  await expect(page.locator('#lnkCrc')).toHaveText('—');
});
