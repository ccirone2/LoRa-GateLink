// The Status tab's one-line verdict (web/js/healthline.js): which problem wins, and what it tells the installer.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { healthLine, linkMargin } from '../../../web/js/healthline.js';

const fx = JSON.parse(readFileSync(new URL('../fixtures/firmware.json', import.meta.url), 'utf8'));
const house = (more = {}) => ({ ...structuredClone(fx.status_common), role: 'house', ...structuredClone(fx.status_house), ...more });
const gate = (more = {}) => ({ ...structuredClone(fx.status_common), role: 'gate', ...structuredClone(fx.status_gate), ...more });
const lvl = (s, ctx) => healthLine(s, ctx).level;
const txt = (s, ctx) => healthLine(s, ctx).text;

test('a healthy site is all good', () => {
  assert.deepEqual(healthLine(house(), { sf: 9 }), { level: 'good', text: 'All good: link up, gate closed.' });
  assert.equal(txt(gate({ gate: 'open' }), { sf: 9 }), 'All good: link up, gate open.');
});

test('setup problems come first: role, a pending reboot, the key, the radio', () => {
  assert.match(txt({ role: 'unset' }), /^Role not set/);
  assert.match(txt(house({ reboot_pending: true, key_set: false })), /waits for a reboot/);
  assert.equal(lvl(house({ key_set: false, link_up: false })), 'bad');
  assert.match(txt(house({ key_set: false, link_up: false })), /^No link key/);
  // A fresh board with its role saved: the reboot, not the role, is what's missing.
  assert.match(txt({ role: 'unset', reboot_pending: true }), /waits for a reboot/);
  assert.match(txt(gate({ radio_ok: false, health: ['radio', 'link'] })), /radio isn’t working/);
});

test('a reboot loop: this board’s, or on the house the gate’s', () => {
  const own = house({ health: ['reboots'], crash_resets: 4, hang_stage: 'hist_save' });
  assert.equal(txt(own), 'This board keeps rebooting (4 crash resets in a row, last hung in hist save): check its supply, then report it.');
  const theirs = house({ health: ['reboots'], crash_resets: 0, remote: { ...fx.status_house.remote, reboot_loop: true } });
  assert.match(txt(theirs), /^The gate keeps rebooting/);
});

test('link down: what differs from the other board, else where to look', () => {
  const down = house({ link_up: false, health: ['link'] });
  assert.match(txt(down), /^No link to the gate: check it has power/);
  const peer = { other: 'gate', there: { fw: '0.13.8' }, diffs: [{ name: 'key id' }, { name: 'sf' }], fwMinor: false };
  assert.equal(txt(down, { peer }), 'The link can’t verify: key id, sf differ from the gate (see Other board). Make them match.');
  const fw = { other: 'gate', there: { fw: '0.15.0' }, diffs: [], fwMinor: true };
  assert.equal(txt(down, { peer: fw }), 'No link to the gate, and its firmware differs (0.13.8 here, 0.15.0 there): update both boards.');
  assert.match(txt(gate({ health: ['link'], link: { verified: false, age_ms: -1 } })), /^No link to the house/);
});

test('the gate’s state and power, the controller, start-up', () => {
  assert.match(txt(house({ gate: 'no_power', health: ['no_power'] })), /^The gate opener has no power/);
  assert.match(txt(house({ gate: 'fault', health: ['fault'] })), /^Both gate limits/);
  assert.equal(lvl(gate({ health: ['ac'], ac_power: false })), 'warn');
  assert.match(txt(gate({ health: ['ac'] })), /on its battery.*Opening is refused.*closing still works/);
  assert.match(txt(house({ ctrl_power: false })), /controller has no power/);
  assert.match(txt(house({ health: ['starting'] })), /waiting for the gate’s first report/);
  assert.match(txt(gate({ health: ['starting'] })), /inputs are settling/);
  // Right after boot the link isn't up yet: starting, not a missing link; after half a minute, the missing link.
  assert.match(txt(house({ health: ['starting', 'link'], link_up: false, uptime_ms: 5000 })), /^Starting up/);
  assert.match(txt(house({ health: ['starting', 'link'], link_up: false, uptime_ms: 60000 })), /^No link to the gate/);
});

test('then firmware, settings last seen, a weak link, the supply, a past hang', () => {
  const fw = { other: 'gate', there: { fw: '0.14.0' }, diffs: [], fwMinor: true };
  assert.equal(txt(house(), { peer: fw }), 'Firmware differs from the gate (0.13.8 here, 0.14.0 there): update both.');
  assert.match(txt(house(), { peer: { other: 'gate', there: {}, diffs: [{ name: 'cr' }], fwMinor: false } }), /Settings differ from the gate as last seen \(cr\)/);
  const weak = house({ link: { ...fx.status_common.link, snr: -5, rssi: -110, noise: -118 } });
  assert.equal(linkMargin(weak, 9), 7.5);
  assert.match(txt(weak, { margin: linkMargin(weak, 9) }), /^Link weak: 7\.5 dB above the floor/);
  assert.equal(lvl(weak), 'good'); // no margin given: nothing to judge
  assert.equal(linkMargin(weak, undefined), null);
  assert.match(txt(house({ supply: false })), /on its LiPo/);
  assert.match(txt(gate({ hang_stage: 'console', hang_pc: 4660 })), /hung in console/);
});

test('the weaker direction sets the margin; a board that never heard the other has none', () => {
  const s = house({ link: { ...fx.status_common.link, snr: 9, rssi: -61, noise: -118 },
    remote: { ...fx.status_house.remote, snr: -10, rssi: -112, noise: -117 } });
  assert.equal(linkMargin(s, 9), 2.5);
  assert.equal(linkMargin(gate({ link: { ...fx.status_common.link, age_ms: -1 } }), 9), null);
});
