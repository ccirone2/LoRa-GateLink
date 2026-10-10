// web/js/keys.js: key ids, the weak-key rule and encrypted key backups (GLKB v1, docs/key-management.md), against
// the firmware's label, the shared test vectors, and a backup written by tools/gatelink.py (Python).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  KEY_ID_LABEL, KDF_ITERATIONS, BackupError, parseKeyHex, keyHex, weakKeyReason, generateKey, keyId, encryptBackup,
  decryptBackup, parseBackup, backupFileName, deriveAesKey, backupAad,
} from '../../../web/js/keys.js';

const root = new URL('../../../', import.meta.url);
const read = (p) => readFileSync(new URL(p, root), 'utf8');
const fx = JSON.parse(read('tests/web/fixtures/firmware.json'));

const KEY = '8a3f1c6e9b2d4f70a1c3e5f7092b4d6f';
const PASS = 'correct horse battery staple';

test('the key id label is the firmware\'s', () => {
  assert.equal(read('firmware/GateLink/config.cpp').match(/KEY_ID_LABEL\[\] = "([^"]+)"/)[1], KEY_ID_LABEL);
});

test('key ids match the shared vectors', async () => {
  for (const [key, id] of fx.key.vectors) assert.equal(await keyId(parseKeyHex(key)), id, key);
});

test('parseKeyHex takes pasted separators; keyHex gives it back', () => {
  assert.equal(keyHex(parseKeyHex('8A:3F:1C:6E 9B-2D-4F-70 a1c3e5f7092b4d6f')), KEY);
  assert.equal(parseKeyHex('8a3f'), null);
  assert.equal(parseKeyHex(`${KEY.slice(0, 31)}g`), null);
});

test('weak keys: the firmware\'s rule', () => {
  const weak = (hex) => weakKeyReason(parseKeyHex(hex));
  assert.match(weak('00000000000000000000000000000000'), /same/);
  assert.match(weak('000102030405060708090a0b0c0d0e0f'), /up by one/);
  assert.match(weak('f8f9fafbfcfdfeff0001020304050607'), /up by one/); // through the wrap
  assert.match(weak('0f0e0d0c0b0a09080706050403020100'), /down by one/);
  assert.match(weak('01020304050607080102030405060708'), /only 8/);
  assert.match(weak('deadbeefdeadbeefdeadbeefdeadbeef'), /only 4/);
  assert.equal(weak('02030405060708090102030405060708'), null); // 9 distinct
  assert.equal(weak('00112233445566778899aabbccddeeff'), null);
  assert.equal(weak('000102030405060708090a0b0c0d0e10'), null);
  assert.equal(weak(KEY), null);
  const k = generateKey();
  assert.equal(k.length, 16);
  assert.equal(weakKeyReason(k), null);
});

test('a backup round-trips, and its file has the format\'s fields', async () => {
  const b = await encryptBackup(parseKeyHex(KEY), PASS, { note: 'bench pair' });
  const text = JSON.stringify(b);
  assert.deepEqual(Object.keys(b), ['format', 'version', 'key_id', 'created', 'note', 'kdf', 'cipher', 'ciphertext']);
  assert.equal(b.format, 'gatelink-key-backup');
  assert.equal(b.version, 1);
  assert.equal(b.key_id, 'e03fddf7');
  assert.match(b.created, /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/);
  assert.deepEqual({ ...b.kdf, salt: undefined }, { name: 'PBKDF2', hash: 'SHA-256', iterations: 600000, salt: undefined });
  assert.equal(atob(b.kdf.salt).length, 16);
  assert.equal(b.cipher.name, 'AES-GCM');
  assert.equal(atob(b.cipher.iv).length, 12);
  assert.equal(atob(b.ciphertext).length, 32);
  assert.ok(!text.includes(KEY), 'the key is not in the file');
  assert.equal(backupFileName(b), 'gatelink-key-e03fddf7.glkey');
  const out = await decryptBackup(text, PASS);
  assert.equal(keyHex(out.key), KEY);
  assert.equal(out.keyId, 'e03fddf7');
  assert.equal(out.note, 'bench pair');
  // A second backup of the same key uses a fresh salt and iv.
  const b2 = await encryptBackup(parseKeyHex(KEY), PASS);
  assert.notEqual(b2.kdf.salt, b.kdf.salt);
  assert.notEqual(b2.ciphertext, b.ciphertext);
  assert.equal(b2.note, undefined);
});

test('a short passphrase is refused', async () => {
  await assert.rejects(encryptBackup(parseKeyHex(KEY), 'short pass'), /at least 12/);
});

test('a wrong passphrase is refused', async () => {
  const b = await encryptBackup(parseKeyHex(KEY), PASS);
  await assert.rejects(decryptBackup(b, `${PASS}!`), (e) => e instanceof BackupError && /wrong passphrase/.test(e.message));
});

test('a changed file is refused', async () => {
  const b = await encryptBackup(parseKeyHex(KEY), PASS);
  const ct = Uint8Array.from(atob(b.ciphertext), (c) => c.charCodeAt(0));
  ct[3] ^= 1;
  const flipped = { ...b, ciphertext: btoa(String.fromCharCode(...ct)) };
  await assert.rejects(decryptBackup(flipped, PASS), /wrong passphrase, or the backup file has been changed/);
  // The key id is authenticated (additional data): a file relabelled to another key's id doesn't open.
  await assert.rejects(decryptBackup({ ...b, key_id: 'fa60d1a7' }, PASS), /has been changed/);
  // Fewer iterations than the format's are refused outright.
  assert.throws(() => parseBackup({ ...b, kdf: { ...b.kdf, iterations: 1000 } }), /iterations/);
  assert.throws(() => parseBackup({ ...b, version: 2 }), /version 2/);
  assert.throws(() => parseBackup({ ...b, format: 'other' }), /not a GateLink key backup/);
  assert.throws(() => parseBackup({ ...b, cipher: { ...b.cipher, iv: btoa('short') } }), /iv must be 12 bytes/);
  assert.throws(() => parseBackup('{not json'), /isn't JSON/);
});

test('a backup whose key doesn\'t have the file\'s key id is refused', async () => {
  // Sealed consistently (the additional data names the wrong id), as a buggy or hostile writer could.
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const aes = await deriveAesKey(PASS, salt, KDF_ITERATIONS);
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: backupAad('fa60d1a7') }, aes,
    parseKeyHex(KEY)));
  const b64 = (u) => btoa(String.fromCharCode(...u));
  const forged = { format: 'gatelink-key-backup', version: 1, key_id: 'fa60d1a7', created: '2026-10-10T00:00:00Z',
    kdf: { name: 'PBKDF2', hash: 'SHA-256', iterations: KDF_ITERATIONS, salt: b64(salt) },
    cipher: { name: 'AES-GCM', iv: b64(iv) }, ciphertext: b64(ct) };
  await assert.rejects(decryptBackup(forged, PASS), /has id e03fddf7, not the fa60d1a7 the file says/);
});

test('reads the backup tools/gatelink.py wrote (and the one this module wrote for the Python test)', async () => {
  // key-backup-v1-python.glkey: tools/gatelink_client/keybackup.py, key 00112233445566778899aabbccddeeff, the
  // passphrase below in NFC; typed here decomposed (u + combining diaeresis), which must not matter.
  const py = await decryptBackup(read('tests/tools/fixtures/key-backup-v1-python.glkey'), 'Gru\u0308ße vom Tor, 2026');
  assert.equal(keyHex(py.key), '00112233445566778899aabbccddeeff');
  assert.equal(py.keyId, 'fa60d1a7');
  const js = await decryptBackup(read('tests/tools/fixtures/key-backup-v1.glkey'), PASS);
  assert.equal(keyHex(js.key), KEY);
});
