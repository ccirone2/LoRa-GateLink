// The Tools tab's firmware card: install the release bundled with the page, or a .bin, over the board's bootloader.
import { $, cmpVer, sleep } from './util.js';
import { S } from './state.js';
import { logLine, toast } from './ui.js';
import { disconnect, stopReconnect, startReconnect, expectBack } from './serial.js';
import { USB_VID, BOOT_PID, APP_START, CHUNK, isBootPort, crc16, parseFirmware, SamBa } from './samba.js';

let bootPick = null; // { resolve, reject } while waiting for the user to grant the bootloader port

function fwStep(text, cls = '') {
  $('fwStep').textContent = text;
  $('fwStep').className = `small ${cls || 'muted'}`;
  if (text) logLine(`firmware: ${text}`, cls === 'bad' ? 'err' : '');
}

function setFlashing(on) {
  S.flashing = on;
  $('btnFwLatest').disabled = on;
  $('fwFile').disabled = on;
  $('btnConnect').disabled = on || !('serial' in navigator);
  $('fwProgress').hidden = !on;
  if (!on) $('fwBootRow').hidden = true;
}

export function updateFwCard() {
  const info = S.boardInfo;
  $('fwBoard').textContent = info ? `${info.fw} (${info.role})` : '—';
  if (!S.latestFw) return;
  const rel = info ? cmpVer(S.latestFw.version, info.fw) : 1;
  const newer = info && rel > 0;
  $('fwLatest').textContent = `${S.latestFw.version}${newer ? ' · update available' : ''}`;
  $('fwLatest').className = newer ? 'good' : '';
  // Only an update is the primary action; reinstalling the same version or going back is offered plainly.
  const b = $('btnFwLatest');
  b.textContent = rel > 0 ? `Install ${S.latestFw.version}` : rel === 0 ? `Reinstall ${S.latestFw.version}` : `Install ${S.latestFw.version} (older)`;
  b.classList.toggle('primary', rel > 0);
}

export async function loadLatest() {
  try {
    const r = await fetch('firmware/latest.json', { cache: 'no-cache' });
    if (!r.ok) return;
    S.latestFw = await r.json();
    $('btnFwLatest').hidden = false;
    updateFwCard();
  } catch { /* served without the bundle (local copy): the file picker still works */ }
}

export async function installLatest() {
  const latest = S.latestFw;
  const r = await fetch(`firmware/${latest.file}`, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`Couldn't download ${latest.file} (${r.status}).`);
  const buf = await r.arrayBuffer();
  if (latest.size !== undefined && buf.byteLength !== latest.size) {
    throw new Error(`${latest.file} is ${buf.byteLength} bytes, not ${latest.size}: the download is damaged. Reload the page and try again.`);
  }
  const sum = [...new Uint8Array(await crypto.subtle.digest('SHA-256', buf))].map((x) => x.toString(16).padStart(2, '0')).join('');
  if (sum !== latest.sha256) throw new Error(`${latest.file} is damaged (checksum mismatch). Reload the page and try again.`);
  const fw = parseFirmware(buf, latest.file);
  if (fw.version !== latest.version) throw new Error(`${latest.file} carries version ${fw.version ?? 'none'}, not ${latest.version}.`);
  await flashFirmware(fw);
}

export async function flashFile(file) {
  const fw = parseFirmware(await file.arrayBuffer(), file.name);
  if (!fw.version) {
    // No marker: another sketch, or GateLink before 0.5.1. Before 0.5.0 config and key lived in program flash, which
    // the update erases, so a board that keeps them on its flash chip would come back without them.
    const loses = S.port && S.boardInfo?.cfg_store === 'spi'
      ? '\n\nIf it is GateLink older than 0.5.0, the board comes back without its settings and key (that firmware keeps '
        + 'them in program flash): export the config first and have the key ready.'
      : '';
    const ask = `${file.name} has no GateLink version marker (firmware before 0.5.1 has none, other sketches neither). Flash it anyway?`;
    if (!confirm(ask + loses)) return;
  }
  await flashFirmware(fw);
}

// Waits for the board to show up in its bootloader. Without a grant for that port (first time on this
// computer) the browser can only offer its chooser from a click, so the user gets a button.
async function findBootPort() {
  for (const until = Date.now() + 5000; Date.now() < until; await sleep(300)) {
    const p = (await navigator.serial.getPorts()).find(isBootPort);
    if (p) return p;
  }
  fwStep('The board is in its bootloader. Click “Select bootloader port” and pick the board (once per computer).');
  $('fwBootRow').hidden = false;
  try {
    return await new Promise((resolve, reject) => { bootPick = { resolve, reject }; });
  } finally {
    bootPick = null;
    $('fwBootRow').hidden = true;
  }
}

export async function pickBootPort() {
  try {
    const p = await navigator.serial.requestPort({ filters: [{ usbVendorId: USB_VID, usbProductId: BOOT_PID }] });
    bootPick?.resolve(p);
  } catch (e) {
    if (e.name === 'NotFoundError') {
      bootPick?.reject(new Error('No bootloader port chosen. The board waits in its bootloader: flash again, or press its reset button once to run the old firmware.'));
    } else bootPick?.reject(e);
  }
}

async function flashFirmware(fw) {
  if (S.flashing) return;
  const label = fw.version ? `firmware ${fw.version}` : fw.name;
  let target = S.port;
  if (S.port) {
    const info = S.boardInfo || {};
    let msg = `Flash ${label} to this ${info.role || ''} board (running ${info.fw || '?'})?`;
    if (fw.version && info.fw && cmpVer(fw.version, info.fw) < 0) msg += '\n\nThat is older than the firmware it runs now.';
    msg += '\n\nRelays release and the link is down for about a minute.';
    if (info.cfg_store !== 'spi') msg += '\n\nThis board keeps config and key in program flash, which the update erases: export the config (Config tab) and have the key ready.';
    else if (S.appliedUnsaved) msg += '\n\nApplied settings that haven’t been saved to flash will be lost.';
    if (!confirm(msg)) return;
  } else {
    // No console session: a board already in its bootloader (double-tapped reset, or a failed update)
    // or one the user picks now.
    target = (await navigator.serial.getPorts()).find(isBootPort);
    if (!target) {
      try {
        target = await navigator.serial.requestPort({ filters: [{ usbVendorId: USB_VID }] });
      } catch (e) {
        if (e.name !== 'NotFoundError') throw e;
        return;
      }
    }
    if (!confirm(`Flash ${label} to the selected board? Relays release and the link is down for about a minute.`)) return;
  }

  setFlashing(true);
  stopReconnect();
  $('fwProgress').value = 0;
  let sb = null;
  try {
    let boot = isBootPort(target) ? target : null;
    if (!boot) {
      if (S.port) await disconnect(true);
      expectBack(target);
      fwStep('Restarting the board into its bootloader…');
      // The firmware's USB serial resets into the bootloader when DTR drops at 1200 baud.
      await target.open({ baudRate: 1200 });
      try {
        await target.setSignals({ dataTerminalReady: true });
        await target.setSignals({ dataTerminalReady: false });
      } finally {
        await target.close().catch(() => {});
      }
      await sleep(500);
      boot = await findBootPort();
    }
    fwStep('Connecting to the bootloader…');
    sb = new SamBa(boot);
    for (let i = 0; ; i++) {
      try {
        await sb.open();
        break;
      } catch (e) {
        if (i >= 10) throw new Error(`Couldn't open the bootloader port: ${e.message}`, { cause: e });
        await sleep(300); // just enumerated; Windows may not have it ready yet
      }
    }
    logLine(`bootloader: ${await sb.hello()}`);
    fwStep('Erasing…');
    await sb.erase(APP_START);
    for (let off = 0; off < fw.bytes.length; off += CHUNK) {
      fwStep(`Writing… ${Math.round((100 * off) / fw.bytes.length)} %`);
      await sb.program(APP_START + off, fw.bytes.subarray(off, off + CHUNK));
      $('fwProgress').value = (off + CHUNK) / fw.bytes.length;
    }
    fwStep('Verifying…');
    const want = crc16(fw.bytes);
    const got = await sb.crc(APP_START, fw.bytes.length);
    if (got !== want) throw new Error(`Verify failed: the board's CRC is ${got.toString(16)}, expected ${want.toString(16)}.`);
    await sb.reset();
    await sb.close();
    sb = null;
    fwStep(`${label} written and verified. Waiting for the board to start…`, 'good');
    S.flashCheck = { version: fw.version, label };
    startReconnect();
  } catch (e) {
    fwStep(`${e.message} If the update had started, the board waits in its bootloader (LED fading): flash again to finish it.`, 'bad');
    throw new Error(`Firmware update failed: ${e.message}`, { cause: e });
  } finally {
    await sb?.close();
    setFlashing(false);
  }
}

// After an update the board comes back on its new firmware: show what it reports.
export function reportFlash(info) {
  const c = S.flashCheck;
  S.flashCheck = null;
  const problems = [];
  if (c.version && info.fw !== c.version) problems.push(`reports firmware ${info.fw}, expected ${c.version}`);
  if (info.role === 'unset') problems.push('has no role');
  if (!info.key_set) problems.push('has no link key');
  if (problems.length) {
    fwStep(`Updated, but the board ${problems.join(', ')}. Restore its config and key.`, 'bad');
    toast(`Board is back but ${problems.join(', ')}.`, 'err');
  } else {
    fwStep(`Updated to ${info.fw}. Role, config and key kept.`, 'good');
    toast(`Firmware ${info.fw} running on the ${info.role} board.`);
  }
}
