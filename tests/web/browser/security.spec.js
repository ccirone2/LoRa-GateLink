// The Security tab: key ids (the board's and the field's), the weak-key check, writing a key and checking the board
// took it, and encrypted backups (save: a download; restore: a file) through the passphrase dialog.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { test, expect, openConsole, connect, requests, fixture } from './helpers.js';
import { decryptBackup, keyHex } from '../../../web/js/keys.js';

const PY_BACKUP = fileURLToPath(new URL('../../tools/fixtures/key-backup-v1-python.glkey', import.meta.url));
const PASS = 'correct horse battery staple';
const DEFAULT_ID = fixture.key.vectors.find(([k]) => k === fixture.key.default)[1];

async function openSecurity(page, boards) {
  await openConsole(page, boards);
  await connect(page);
  await page.locator('#tabbtn-security').click();
}

test('shows the board\'s key id, and whether the key in the field is the same', async ({ page }) => {
  await openSecurity(page);
  await expect(page.locator('#secKeyId')).toHaveText(DEFAULT_ID);
  await page.locator('#keyInput').fill(fixture.key.default.toUpperCase().match(/../g).join(':'));
  await expect(page.locator('#keyHint')).toHaveText(`Key id ${DEFAULT_ID}, the same as this board’s.`);
  await page.locator('#keyInput').fill('00112233445566778899aabbccddeeff');
  await expect(page.locator('#keyHint')).toHaveText(`Key id fa60d1a7; this board has a different key (${DEFAULT_ID}).`);
});

test('a board without a key says so; disconnected, no id is shown', async ({ page }) => {
  await openSecurity(page, [{ role: 'house', keySet: false }]);
  await expect(page.locator('#secKeyId')).toHaveText('no key');
  await page.locator('#btnDisconnect').click();
  await expect(page.locator('#secKeyId')).toHaveText('—');
});

test('firmware before key ids: the write is reported without a check', async ({ page }) => {
  await openSecurity(page, [{ role: 'house', fw: '0.13.5' }]);
  await expect(page.locator('#secKeyId')).toHaveText('firmware too old to say');
  await page.locator('#keyInput').fill('00112233445566778899aabbccddeeff');
  await expect(page.locator('#keyHint')).toHaveText('Key id fa60d1a7.');
  await page.locator('#btnKeySet').click();
  await expect(page.locator('#toast')).toContainText('can’t report key ids');
});

test('writing a key checks the board reports its id', async ({ page }) => {
  await openSecurity(page);
  await page.locator('#keyInput').fill('00112233445566778899aabbccddeeff');
  await page.locator('#btnKeySet').click();
  await expect(page.locator('#toast')).toHaveText(
    'Key written and saved: the board reports key id fa60d1a7, this key’s. Write the same key to the other board.');
  await expect(page.locator('#secKeyId')).toHaveText('fa60d1a7');
  expect(page.dialogs[0]).toContain('already has a key');
  // A board that doesn't take it (the reply says ok, but its id stays the old one) is flagged.
  await page.evaluate(() => {
    const b = window.__fake.boards[0];
    const handle = b.handle.bind(b);
    b.handle = (req) => (req.cmd === 'key.set' ? { id: req.id, ok: true } : handle(req));
  });
  await page.locator('#keyInput').fill('0f1e2d3c4b5a69788796a5b4c3d2e1f0');
  await page.locator('#btnKeySet').click();
  await expect(page.locator('#toast')).toHaveText('The board reports key id fa60d1a7, not 2fa26167: the key didn’t take. Write it again.');
});

test('a weak key is flagged and not written', async ({ page }) => {
  await openSecurity(page);
  await page.locator('#keyInput').fill('000102030405060708090a0b0c0d0e0f');
  await expect(page.locator('#keyInput')).toHaveAttribute('aria-invalid', 'true');
  await expect(page.locator('#keyHint')).toContainText('Weak key: the bytes count up by one');
  await page.locator('#btnKeySet').click();
  await expect(page.locator('#toast')).toContainText('Weak key (the bytes count up by one)');
  await page.locator('#keyInput').fill('deadbeefdeadbeefdeadbeefdeadbeef');
  await expect(page.locator('#keyHint')).toContainText('only 4 different byte values');
  expect((await requests(page)).map((r) => r.cmd)).not.toContain('key.set');
  // The fake board refuses one too, as the firmware does.
  const res = await page.evaluate(() => window.__fake.boards[0].handle({ id: 99, cmd: 'key.set', key: '00'.repeat(16) }));
  expect(res).toEqual({ id: 99, ok: false, error: fixture.key.weak_error });
});

test('Generate makes a key with an id; Save encrypted backup downloads it, encrypted', async ({ page }) => {
  await openSecurity(page);
  await page.locator('#btnKeyGen').click();
  const key = await page.locator('#keyInput').inputValue();
  await expect(page.locator('#keyHint')).toHaveText(/^Key id [0-9a-f]{8}; this board has a different key/);
  const id = (await page.locator('#keyHint').textContent()).slice(7, 15);

  await page.locator('#btnKeyBackup').click();
  await expect(page.locator('#passDialog')).toBeVisible();
  await expect(page.locator('#passWarn')).toContainText('Nothing can recover a forgotten passphrase');
  await page.locator('#passOne').fill('too short');
  await page.locator('#btnPassOk').click();
  await expect(page.locator('#passError')).toHaveText('At least 12 characters, please (9 so far).');
  await page.locator('#passOne').fill(PASS);
  await page.locator('#passTwo').fill(`${PASS}.`);
  await page.locator('#btnPassOk').click();
  await expect(page.locator('#passError')).toHaveText('The two passphrases differ.');
  await page.locator('#passTwo').fill(PASS);
  await page.locator('#passNote').fill('bench pair');
  const [download] = await Promise.all([page.waitForEvent('download'), page.locator('#btnPassOk').click()]);
  await expect(page.locator('#passDialog')).toBeHidden();
  expect(download.suggestedFilename()).toBe(`gatelink-key-${id}.glkey`);
  const text = readFileSync(await download.path(), 'utf8');
  expect(text).not.toContain(key);
  const backup = JSON.parse(text);
  expect(backup).toMatchObject({ format: 'gatelink-key-backup', version: 1, key_id: id, note: 'bench pair' });
  const out = await decryptBackup(text, PASS);
  expect(keyHex(out.key)).toBe(key);
  await expect(page.locator('#toast')).toContainText(`Backup of key ${id} saved as gatelink-key-${id}.glkey`);
  // Nothing went to the board.
  expect((await requests(page)).map((r) => r.cmd)).not.toContain('key.set');
});

test('cancelling the passphrase dialog saves nothing', async ({ page }) => {
  await openSecurity(page);
  await page.locator('#keyInput').fill(fixture.key.default);
  let downloads = 0;
  page.on('download', () => downloads++);
  await page.locator('#btnKeyBackup').click();
  await page.locator('#passOne').fill(PASS);
  await page.locator('#btnPassCancel').click();
  await expect(page.locator('#passDialog')).toBeHidden();
  await page.locator('#btnKeyBackup').click();
  await expect(page.locator('#passOne')).toHaveValue(''); // not kept from last time
  await page.keyboard.press('Escape');
  await expect(page.locator('#passDialog')).toBeHidden();
  await page.waitForTimeout(300);
  expect(downloads).toBe(0);
});

test('Restore from backup fills the key field (a Python-made backup); Write then sets it', async ({ page }) => {
  await openSecurity(page);
  await page.locator('#keyRestoreFile').setInputFiles(PY_BACKUP);
  await expect(page.locator('#passDialog')).toBeVisible();
  await expect(page.locator('#passIntro')).toContainText('key-backup-v1-python.glkey: key fa60d1a7');
  await expect(page.locator('#passTwoRow')).toBeHidden();
  await page.locator('#passOne').fill('not the passphrase');
  await page.locator('#btnPassOk').click();
  await expect(page.locator('#passError')).toHaveText('wrong passphrase, or the backup file has been changed');
  await page.locator('#passOne').fill('Grüße vom Tor, 2026');
  await page.locator('#btnPassOk').click();
  await expect(page.locator('#passDialog')).toBeHidden();
  await expect(page.locator('#keyInput')).toHaveValue('00112233445566778899aabbccddeeff');
  await expect(page.locator('#toast')).toContainText('Key fa60d1a7 restored from the backup');
  await page.locator('#btnKeySet').click();
  await expect(page.locator('#secKeyId')).toHaveText('fa60d1a7');
  expect((await requests(page)).find((r) => r.cmd === 'key.set').key).toBe('00112233445566778899aabbccddeeff');
});

test('a file that isn\'t a backup is refused before any passphrase', async ({ page }) => {
  await openSecurity(page);
  await page.locator('#keyRestoreFile').setInputFiles({ name: 'config.json', mimeType: 'application/json',
    buffer: Buffer.from('{"params":{}}') });
  await expect(page.locator('#toast')).toHaveText('config.json: not a GateLink key backup');
  await expect(page.locator('#passDialog')).toBeHidden();
});

test('backups work without a board', async ({ page }) => {
  await openConsole(page);
  await page.locator('#tabbtn-security').click();
  await expect(page.locator('#btnKeyBackup')).toBeEnabled();
  await expect(page.locator('#keyRestoreFile')).toBeEnabled();
  await expect(page.locator('#btnKeySet')).toBeDisabled();
  await page.locator('#keyRestoreFile').setInputFiles(PY_BACKUP);
  await page.locator('#passOne').fill('Grüße vom Tor, 2026');
  await page.locator('#btnPassOk').click();
  await expect(page.locator('#keyInput')).toHaveValue('00112233445566778899aabbccddeeff');
  await expect(page.locator('#keyHint')).toHaveText('Key id fa60d1a7.');
});

test('the board picker shows each board\'s key id', async ({ page }) => {
  await openConsole(page, [{ role: 'house' }, { role: 'gate', key: '00112233445566778899aabbccddeeff' }]);
  await page.locator('#btnConnect').click();
  await expect(page.locator('#pickList')).toContainText(`key ${DEFAULT_ID}`);
  await expect(page.locator('#pickList')).toContainText('key fa60d1a7');
});
