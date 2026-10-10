// The Config tab: the form, Apply (in chunks), Save, must-match settings, import and export.
import { test, expect } from '@playwright/test';
import { openConsole, connect, requests } from './helpers.js';

async function openConfig(page, boards, options) {
  await openConsole(page, boards, options);
  await connect(page, boards?.[0]?.role ?? 'house');
  await page.locator('#tabbtn-config').click();
}

test('the form shows every setting in its group, with on/off ones as toggles', async ({ page }) => {
  await openConfig(page);
  await expect(page.locator('#cfgForm .card .label')).toHaveText(['General', 'Radio', 'Link', 'Inputs', 'Gate node',
    'House node', 'Board']);
  await expect(page.locator('#p_heartbeat_s')).toHaveValue('30');
  await expect(page.locator('#p_power_sense')).toHaveAttribute('type', 'checkbox');
  await expect(page.locator('#p_power_sense')).toBeChecked();
  await expect(page.locator('#p_bw_hz')).toHaveValue('500000');
});

test('Apply sends only the edited settings, and Save is offered afterwards', async ({ page }) => {
  await openConfig(page);
  await page.locator('#p_heartbeat_s').fill('45');
  await page.locator('#p_linkloss_open').uncheck();
  await expect(page.locator('#btnCfgApply')).toHaveText('Apply (2)');
  await expect(page.locator('#tabbtn-config')).toHaveClass(/pending/);
  await page.locator('#btnCfgApply').click();
  await expect(page.locator('#toast')).toContainText('Applied heartbeat_s, linkloss_open');
  const set = (await requests(page)).filter((r) => r.cmd === 'config.set');
  expect(set).toHaveLength(1);
  expect(set[0].params).toEqual({ heartbeat_s: 45, linkloss_open: 0 });
  await expect(page.locator('#btnCfgSave')).toHaveText('Save to flash •');
  await page.locator('#btnCfgSave').click();
  await expect(page.locator('#toast')).toHaveText('Saved to flash.');
  await expect(page.locator('#btnCfgSave')).toHaveText('Save to flash');
  expect((await requests(page)).some((r) => r.cmd === 'config.save')).toBe(true);
});

test('an out-of-range value is refused before anything is sent', async ({ page }) => {
  await openConfig(page);
  await page.locator('#p_heartbeat_s').fill('2');
  await expect(page.locator('#p_heartbeat_s')).toHaveAttribute('aria-invalid', 'true');
  await page.locator('#btnCfgApply').click();
  await expect(page.locator('#toast')).toContainText('heartbeat_s must be an integer 5–3600');
  expect((await requests(page)).some((r) => r.cmd === 'config.set')).toBe(false);
});

test('a must-match radio setting asks first, and nothing is sent if declined', async ({ page }) => {
  await openConfig(page, [{ role: 'house' }], { dismissDialogs: true });
  await page.locator('#p_sf').fill('10');
  await page.locator('#btnCfgApply').click();
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('Changing sf takes the link down');
  expect((await requests(page)).some((r) => r.cmd === 'config.set')).toBe(false);
});

test('Save is refused while edits are unapplied', async ({ page }) => {
  await openConfig(page);
  await page.locator('#p_pulse_ms').fill('600');
  await page.locator('#btnCfgSave').click();
  await expect(page.locator('#toast')).toContainText('isn’t applied yet');
});

test('importing the other board’s export keeps this board’s role', async ({ page }) => {
  await openConfig(page);
  const file = { name: 'gate.json', mimeType: 'application/json',
    buffer: Buffer.from(JSON.stringify({ role: 'gate', params: { role: 2, heartbeat_s: 60, pulse_ms: 700 } })) };
  await page.locator('#fileImport').setInputFiles(file);
  await expect(page.locator('#toast')).toContainText('Kept this board’s role (house)');
  await expect(page.locator('#p_role')).toHaveValue('1');
  await expect(page.locator('#p_heartbeat_s')).toHaveValue('60');
  await expect(page.locator('#btnCfgApply')).toHaveText('Apply (2)');
});

test('export writes what the board runs, not unapplied edits', async ({ page }) => {
  await openConfig(page);
  await page.locator('#p_pulse_ms').fill('600');
  const [download] = await Promise.all([page.waitForEvent('download'), page.locator('#btnCfgExport').click()]);
  const json = JSON.parse(await (await download.createReadStream()).toArray().then((c) => Buffer.concat(c).toString()));
  expect(json.role).toBe('house');
  expect(json.params.pulse_ms).toBe(500);
  await expect(page.locator('#toast')).toContainText('unapplied edit isn’t included');
});

test('a board that stops answering mid-apply leaves the rest as unapplied edits', async ({ page }) => {
  await openConfig(page);
  // Nine edits: two chunks. The board answers the first config.set, then goes silent.
  const edits = { heartbeat_s: 40, link_timeout_s: 120, cmd_ttl_s: 12, debounce_ms: 60, pulse_ms: 600, travel_timeout_s: 70,
    sync_window_ms: 3500, resync_ms: 1200, mismatch_timeout_s: 80 };
  for (const [k, v] of Object.entries(edits)) await page.locator(`#p_${k}`).fill(String(v));
  await page.evaluate(() => {
    const b = window.__fake.boards[0];
    const handle = b.handle.bind(b);
    let sets = 0;
    b.handle = (req) => (req.cmd === 'config.set' && ++sets === 2 ? null : handle(req));
  });
  await page.locator('#btnCfgApply').click();
  await expect(page.locator('#toast')).toContainText('Applying stopped', { timeout: 10000 });
  await expect(page.locator('#btnCfgApply')).toHaveText('Apply (1)');
  await expect(page.locator('#btnCfgSave')).toHaveText('Save to flash •');
});
