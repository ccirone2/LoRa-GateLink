// Boards that don't match (ROADMAP F10, web/js/peer.js): the page remembers each board it connects (firmware, key
// id, the must-match settings) and, with the other one connected, says what differs next to "Peer verified".
import { test, expect, openConsole, connect, requests } from './helpers.js';

// Connects the house, then the gate (boards[0] and boards[1]), as an installer going from one box to the other.
async function houseThenGate(page, house, gate) {
  await openConsole(page, [{ role: 'house', ...house }, { role: 'gate', ...gate }]);
  await connect(page, 'house');
  await expect(page.locator('#lnkPeer')).not.toHaveText('—');
  await page.locator('#btnDisconnect').click();
  await expect(page.locator('#devline')).toHaveText('not connected');
  await connect(page, 'gate');
}

test('before the other board has been seen, the house asks the gate for its firmware', async ({ page }) => {
  await openConsole(page, [{ role: 'house' }]);
  await connect(page);
  await expect(page.locator('#lnkPeer')).toHaveText('gate fw 0.13.8; its settings not seen yet: connect it once to compare');
  expect((await requests(page)).filter((r) => r.cmd === 'remote.diag')).toHaveLength(1);
});

test('the house with its link down asks nothing, and a gate alone has nothing to compare', async ({ page }) => {
  await openConsole(page, [{ role: 'house', status: { link_up: false } }, { role: 'gate' }]);
  await connect(page, 'house');
  await expect(page.locator('#lnkPeer')).toHaveText('gate not seen yet: connect it once to compare');
  expect((await requests(page)).map((r) => r.cmd)).not.toContain('remote.diag');
});

test('boards that match say so', async ({ page }) => {
  await houseThenGate(page, {}, {});
  await expect(page.locator('#lnkPeer')).toContainText('matches · house fw 0.13.8 · house seen');
});

test('a must-match setting and the key differ: the link can’t verify', async ({ page }) => {
  await houseThenGate(page, {}, {
    params: { sf: 10, freq_hz: 915200000 }, key: '00112233445566778899aabbccddeeff',
    status: { link: { verified: false, age_ms: -1 } },
  });
  const line = page.locator('#lnkPeer');
  await expect(line).toContainText('the link can’t verify, these differ: key id fa60d1a7 here,');
  await expect(line).toContainText('freq_hz 915.2 MHz here, 915 MHz on the house');
  await expect(line).toContainText('sf 10 here, 9 on the house');
  await expect(line.locator('.bad')).toHaveCount(1);
});

test('firmware a MINOR apart says to update both', async ({ page }) => {
  await houseThenGate(page, { fw: '0.14.0' }, { fw: '0.13.9' });
  await expect(page.locator('#lnkPeer')).toContainText('differs: firmware 0.13.9 here, 0.14.0 on the house: update both');
});

test('firmware a PATCH apart matches', async ({ page }) => {
  await houseThenGate(page, { fw: '0.13.9' }, { fw: '0.13.8' });
  await expect(page.locator('#lnkPeer')).toContainText('matches · house fw 0.13.9');
});

test('a board without a key differs from one with a key', async ({ page }) => {
  await houseThenGate(page, {}, { keySet: false });
  await expect(page.locator('#lnkPeer')).toContainText('differs: key id none here,');
});
