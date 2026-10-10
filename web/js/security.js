// The Security tab: the key field and its id, the board's key id, and encrypted key backups (the crypto is in
// keys.js). Writing the key is wired in app.js (it needs the board connection).
import { $, download } from './util.js';
import { S } from './state.js';
import { toast } from './ui.js';
import {
  MIN_PASSPHRASE, parseKeyHex, keyHex, weakKeyReason, generateKey, keyId, encryptBackup, decryptBackup, parseBackup,
  backupFileName, passphraseLength,
} from './keys.js';

// The field as typed: spaces, colons and dashes (pasted keys) dropped.
export const keyValue = () => $('keyInput').value.replace(/[\s:-]/g, '').toLowerCase();

// What the connected board says about its key (info.key_id from firmware 0.13.8).
function boardKeyText(info) {
  if (!info) return '—';
  if (!info.key_set) return 'no key';
  return info.key_id ?? 'firmware too old to say';
}

export function showBoardKey() {
  $('secKeyId').textContent = boardKeyText(S.boardInfo);
  checkKeyInput();
}

// The hint under the field: what's wrong with it, or its key id and whether the board holds the same key.
let hintSeq = 0;
export async function checkKeyInput() {
  const seq = ++hintSeq;
  const k = keyValue();
  const bytes = parseKeyHex(k);
  const weak = bytes && weakKeyReason(bytes);
  $('keyInput').setAttribute('aria-invalid', String(!!k && (!bytes || !!weak)));
  if (!bytes) {
    $('keyHint').textContent = !k ? '' : `${k.length}/32 characters${/[^0-9a-f]/.test(k) ? ', hex digits only (0–9, a–f)' : ''}`;
    return;
  }
  if (weak) {
    $('keyHint').textContent = `Weak key: ${weak}. Boards refuse it (firmware 0.13.8 and later); generate one instead.`;
    return;
  }
  const id = await keyId(bytes);
  if (seq !== hintSeq) return; // typed on meanwhile
  const info = S.boardInfo;
  let board = '';
  if (info && !info.key_set) board = '; this board has no key yet';
  else if (info?.key_id) board = info.key_id === id ? ', the same as this board’s' : `; this board has a different key (${info.key_id})`;
  $('keyHint').textContent = `Key id ${id}${board}.`;
}

export function generate() {
  $('keyInput').value = keyHex(generateKey());
  checkKeyInput();
}

export async function copyKey() {
  const key = keyValue();
  if (!key) return toast('Nothing to copy: generate or enter a key first.', 'err');
  try {
    await navigator.clipboard.writeText(key);
    toast('Key copied to the clipboard.');
  } catch (e) {
    toast(`Copy failed: ${e.message}`, 'err');
  }
}

// The key in the field, checked for writing: { hex, id }, or null after saying why it can't be written.
export async function keyToWrite() {
  const bytes = parseKeyHex(keyValue());
  if (!bytes) {
    toast('Key must be exactly 32 hex characters.', 'err');
    return null;
  }
  const weak = weakKeyReason(bytes);
  if (weak) {
    toast(`Weak key (${weak}): boards refuse it. Generate one instead.`, 'err');
    return null;
  }
  return { hex: keyHex(bytes), id: await keyId(bytes) };
}

// After key.set and a fresh info: does the board report the id of the key written?
export function reportKeyWritten(id) {
  const got = S.boardInfo?.key_id;
  if (got === undefined) {
    toast('Key written and saved. This firmware can’t report key ids (0.13.8 and later do), so check the link comes up. '
      + 'Write the same key to the other board.');
  } else if (got === id) {
    toast(`Key written and saved: the board reports key id ${id}, this key’s. Write the same key to the other board.`);
  } else {
    toast(`The board reports key id ${got ?? 'none'}, not ${id}: the key didn’t take. Write it again.`, 'err');
  }
}

// The passphrase dialog. mode 'save': twice, at least MIN_PASSPHRASE characters, plus an optional note; 'restore':
// once. `work(passphrase, note)` runs with the dialog still open (it can take a second); if it throws, its message
// shows in the dialog and the passphrase can be typed again. Resolves true once work succeeded, false if cancelled.
function askPassphrase({ mode, title, intro, work }) {
  const save = mode === 'save';
  const dlg = $('passDialog');
  $('passTitle').textContent = title;
  $('passIntro').textContent = intro;
  $('passTwoRow').hidden = !save;
  $('passNoteRow').hidden = !save;
  $('passWarn').textContent = save
    ? `At least ${MIN_PASSPHRASE} characters. Nothing can recover a forgotten passphrase, and without it the backup is `
      + 'useless: store it in a password manager, apart from the file.'
    : '';
  for (const id of ['passOne', 'passTwo', 'passNote']) $(id).value = '';
  $('passOne').autocomplete = save ? 'new-password' : 'current-password';
  $('passError').textContent = '';
  $('btnPassOk').textContent = save ? 'Encrypt and save' : 'Decrypt';
  return new Promise((resolve) => {
    let busy = false;
    const finish = (ok) => {
      $('passForm').onsubmit = null;
      $('btnPassCancel').onclick = null;
      dlg.removeEventListener('cancel', onCancel);
      for (const id of ['passOne', 'passTwo']) $(id).value = '';
      if (dlg.open) dlg.close();
      resolve(ok);
    };
    const onCancel = (e) => {
      if (busy) e.preventDefault();
      else finish(false);
    };
    dlg.addEventListener('cancel', onCancel); // Escape
    $('btnPassCancel').onclick = () => { if (!busy) finish(false); };
    $('passForm').onsubmit = async (e) => {
      e.preventDefault();
      if (busy) return;
      const pass = $('passOne').value;
      const err = (msg) => {
        $('passError').textContent = msg;
        $('passOne').focus();
      };
      if (save && passphraseLength(pass) < MIN_PASSPHRASE) return err(`At least ${MIN_PASSPHRASE} characters, please (${passphraseLength(pass)} so far).`);
      if (save && pass !== $('passTwo').value) return err('The two passphrases differ.');
      if (!pass) return err('Enter the passphrase.');
      busy = true;
      $('btnPassOk').disabled = true;
      $('btnPassCancel').disabled = true;
      $('passError').textContent = '';
      const label = $('btnPassOk').textContent;
      $('btnPassOk').textContent = save ? 'Encrypting…' : 'Decrypting…';
      try {
        await work(pass, $('passNote').value.trim());
        busy = false;
        finish(true);
      } catch (ex) {
        busy = false;
        err(ex.message);
      } finally {
        $('btnPassOk').disabled = false;
        $('btnPassCancel').disabled = false;
        $('btnPassOk').textContent = label;
      }
    };
    dlg.showModal();
    $('passOne').focus();
  });
}

export async function saveBackup() {
  const bytes = parseKeyHex(keyValue());
  if (!bytes) return toast('Nothing to back up: generate, restore or enter a key first.', 'err');
  const weak = weakKeyReason(bytes);
  if (weak) return toast(`Not backing up a weak key (${weak}): generate a new one.`, 'err');
  const id = await keyId(bytes);
  let backup = null;
  const ok = await askPassphrase({
    mode: 'save',
    title: 'Save encrypted backup',
    intro: `Key ${id} is saved encrypted with this passphrase (AES-GCM, key from PBKDF2-SHA256), as gatelink-key-${id}.glkey.`,
    work: async (pass, note) => { backup = await encryptBackup(bytes, pass, { note }); },
  });
  if (!ok) return;
  download(backupFileName(backup), `${JSON.stringify(backup, null, 2)}\n`, 'application/json');
  toast(`Backup of key ${id} saved as ${backupFileName(backup)}. Keep its passphrase in a password manager: it can’t be recovered.`);
}

export async function restoreBackup(file) {
  let backup;
  try {
    backup = parseBackup(await file.text());
  } catch (e) {
    return toast(`${file.name}: ${e.message}`, 'err');
  }
  const made = backup.created ? `, made ${String(backup.created).slice(0, 10)}` : '';
  const note = backup.note ? ` (“${backup.note}”)` : '';
  let out = null;
  const ok = await askPassphrase({
    mode: 'restore',
    title: 'Restore key from backup',
    intro: `${file.name}: key ${backup.key_id}${made}${note}. Its key goes into the key field; Write to board then sets it.`,
    work: async (pass) => { out = await decryptBackup(backup, pass); },
  });
  if (!ok) return;
  $('keyInput').value = keyHex(out.key);
  await checkKeyInput();
  toast(`Key ${out.keyId} restored from the backup into the key field. Write it to the board to use it.`);
}
