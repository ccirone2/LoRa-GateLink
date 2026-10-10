// The link key's lifecycle, without the page: key ids, the weak-key rule, and encrypted backups ("GLKB v1",
// docs/key-management.md). Pure WebCrypto, so it runs the same in the page and under Node (tests/web/unit), and its
// files are the ones tools/gatelink.py reads and writes (tools/gatelink_client/keybackup.py).

export const KEY_ID_LABEL = 'GateLink key id v1'; // firmware config.cpp
export const BACKUP_FORMAT = 'gatelink-key-backup';
export const BACKUP_VERSION = 1;
export const KDF_ITERATIONS = 600000; // what a backup is written with
export const KDF_MAX_ITERATIONS = 10000000; // the most a backup may ask for, so a crafted file can't hang the page
export const MIN_PASSPHRASE = 12; // characters
export const BACKUP_EXT = '.glkey';

const subtle = () => globalThis.crypto.subtle;
const utf8 = (s) => new TextEncoder().encode(s);

export class BackupError extends Error {}

// Hex text as typed or pasted (spaces, colons and dashes allowed) -> 16 key bytes, or null.
export function parseKeyHex(text) {
  const hex = String(text).replace(/[\s:-]/g, '').toLowerCase();
  if (!/^[0-9a-f]{32}$/.test(hex)) return null;
  return Uint8Array.from(hex.match(/../g), (b) => parseInt(b, 16));
}

export const keyHex = (bytes) => [...bytes].map((x) => x.toString(16).padStart(2, '0')).join('');

// Why key.set would refuse this key (firmware 0.13.8, configKeyWeak), or null if it wouldn't: all 16 bytes equal,
// bytes counting up or down by one (mod 256), or 8 or fewer distinct byte values.
export function weakKeyReason(bytes) {
  const distinct = new Set(bytes).size;
  if (distinct === 1) return 'all 16 bytes are the same';
  const steps = new Set([...bytes].slice(1).map((b, i) => (b - bytes[i] + 256) % 256));
  if (steps.size === 1 && (steps.has(1) || steps.has(255))) return `the bytes count ${steps.has(1) ? 'up' : 'down'} by one`;
  if (distinct <= 8) return `only ${distinct} different byte values (at least 9 needed)`;
  return null;
}

// A fresh random key (redrawn in the ~1 in 10^10 case that it is weak).
export function generateKey() {
  for (;;) {
    const k = globalThis.crypto.getRandomValues(new Uint8Array(16));
    if (!weakKeyReason(k)) return k;
  }
}

// The key's id: the first 4 bytes of HMAC-SHA256(key, "GateLink key id v1"), 8 lowercase hex digits, as the
// firmware reports it (info, config.get). It tells keys apart without revealing them.
export async function keyId(bytes) {
  const k = await subtle().importKey('raw', bytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const mac = new Uint8Array(await subtle().sign('HMAC', k, utf8(KEY_ID_LABEL)));
  return keyHex(mac.slice(0, 4));
}

const b64 = (bytes) => btoa(String.fromCharCode(...bytes));
function unb64(text, what, len) {
  let bin;
  try {
    bin = atob(String(text));
  } catch {
    throw new BackupError(`not a key backup: ${what} isn't base64`);
  }
  if (len !== undefined && bin.length !== len) throw new BackupError(`not a key backup: ${what} must be ${len} bytes`);
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
}

// Passphrases are NFC-normalized UTF-8, so one typed on another system (or in Python) gives the same bytes.
export const passphraseBytes = (p) => utf8(String(p).normalize('NFC'));
export const passphraseLength = (p) => [...String(p).normalize('NFC')].length;

// The AES-256-GCM key for a backup: PBKDF2-HMAC-SHA256 over the passphrase.
export async function deriveAesKey(passphrase, salt, iterations) {
  const base = await subtle().importKey('raw', passphraseBytes(passphrase), 'PBKDF2', false, ['deriveKey']);
  return subtle().deriveKey({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations }, base,
    { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

// Additional authenticated data: binds the ciphertext to the key id in the file.
export const backupAad = (id) => utf8(`${BACKUP_FORMAT} v${BACKUP_VERSION} ${id}`);

const isoNow = () => new Date().toISOString().replace(/\.\d{3}Z$/, 'Z');

// Encrypts the 16 key bytes under the passphrase: the backup as an object (JSON.stringify it to save).
export async function encryptBackup(bytes, passphrase, { note = '', created = isoNow() } = {}) {
  if (!(bytes instanceof Uint8Array) || bytes.length !== 16) throw new BackupError('the key must be 16 bytes');
  if (passphraseLength(passphrase) < MIN_PASSPHRASE) throw new BackupError(`the passphrase needs at least ${MIN_PASSPHRASE} characters`);
  const id = await keyId(bytes);
  const salt = globalThis.crypto.getRandomValues(new Uint8Array(16));
  const iv = globalThis.crypto.getRandomValues(new Uint8Array(12));
  const aes = await deriveAesKey(passphrase, salt, KDF_ITERATIONS);
  const ct = new Uint8Array(await subtle().encrypt({ name: 'AES-GCM', iv, additionalData: backupAad(id) }, aes, bytes));
  const backup = { format: BACKUP_FORMAT, version: BACKUP_VERSION, key_id: id, created };
  if (note) backup.note = String(note);
  return {
    ...backup,
    kdf: { name: 'PBKDF2', hash: 'SHA-256', iterations: KDF_ITERATIONS, salt: b64(salt) },
    cipher: { name: 'AES-GCM', iv: b64(iv) },
    ciphertext: b64(ct),
  };
}

export const backupFileName = (backup) => `gatelink-key-${backup.key_id}${BACKUP_EXT}`;

// A backup's text (or parsed object) checked for shape, before asking for its passphrase. Throws BackupError.
export function parseBackup(input) {
  let b = input;
  if (typeof input === 'string') {
    try {
      b = JSON.parse(input);
    } catch {
      throw new BackupError('not a key backup: the file isn\'t JSON');
    }
  }
  if (!b || typeof b !== 'object' || b.format !== BACKUP_FORMAT) throw new BackupError('not a GateLink key backup');
  if (b.version !== BACKUP_VERSION) throw new BackupError(`key backup version ${b.version} isn't supported (this page reads version ${BACKUP_VERSION})`);
  if (typeof b.key_id !== 'string' || !/^[0-9a-f]{8}$/.test(b.key_id)) throw new BackupError('not a key backup: bad key_id');
  const kdf = b.kdf || {};
  if (kdf.name !== 'PBKDF2' || kdf.hash !== 'SHA-256') throw new BackupError('not a key backup: unsupported kdf');
  if (!Number.isInteger(kdf.iterations) || kdf.iterations < KDF_ITERATIONS || kdf.iterations > KDF_MAX_ITERATIONS) {
    throw new BackupError(`not a key backup: kdf iterations must be ${KDF_ITERATIONS} to ${KDF_MAX_ITERATIONS}`);
  }
  if (b.cipher?.name !== 'AES-GCM') throw new BackupError('not a key backup: unsupported cipher');
  unb64(kdf.salt, 'the salt', 16);
  unb64(b.cipher.iv, 'the iv', 12);
  unb64(b.ciphertext, 'the ciphertext', 32); // 16 key bytes + the 16-byte GCM tag
  return b;
}

// Decrypts a backup: { key (16 bytes), keyId, created, note }. Throws BackupError for a wrong passphrase or a changed
// file (GCM can't tell them apart), and if the key inside doesn't have the file's key id.
export async function decryptBackup(input, passphrase) {
  const b = parseBackup(input);
  const aes = await deriveAesKey(passphrase, unb64(b.kdf.salt), b.kdf.iterations);
  let key;
  try {
    key = new Uint8Array(await subtle().decrypt({ name: 'AES-GCM', iv: unb64(b.cipher.iv), additionalData: backupAad(b.key_id) },
      aes, unb64(b.ciphertext)));
  } catch {
    throw new BackupError('wrong passphrase, or the backup file has been changed');
  }
  const id = await keyId(key);
  if (id !== b.key_id) throw new BackupError(`the key in the backup has id ${id}, not the ${b.key_id} the file says`);
  return { key, keyId: id, created: b.created ?? null, note: b.note ?? null };
}
