// The board's console over Web Serial: the port, newline-delimited JSON requests and replies (docs/console.md), the
// board picker, and auto-reconnect after a reboot or a dropped cable. What happens on the page when a board connects,
// goes or sends an event is up to the hooks (set by app.js), so this module knows nothing of the tabs.
import { $, esc, cap } from './util.js';
import { S } from './state.js';
import { logLine, toast } from './ui.js';
import { USB_VID, BOOT_PID, isBootPort } from './samba.js';

export const hooks = {
  onOpen: async () => {}, // a board is connected: fill the page ({ otherBoard }: not the one connected last)
  onClosing: () => {}, // it's going: stop timers that talk to it
  onClosed: () => {}, // it's gone
  onEvent: () => {}, // an unsolicited {"event": ...} line
  onRawLine: () => {}, // every line, before it's parsed
};

let writer = null;
let reader = null;
let nextId = 1;
const pending = new Map();
// Stream pipes to/from the port; they must finish (unlocking the port streams) before port.close() works.
let readPipe = null;
let writePipe = null;

// Auto-reconnect: after a reboot or unexpected drop the board re-enumerates; reopen it (no re-pairing
// needed for an already-granted port) for a while instead of making the user click Connect again.
const RECONNECT_MS = 30000;
let lastPort = null;
let reconnectUntil = 0;
let reconnectTimer = null;
let opening = false;
// The board that went away (boardIdentity()): any granted port that comes back in the window is reopened,
// and it may be the other board.
let expectBoard = null;

export async function connect() {
  stopReconnect();
  if (!(await knownPorts()).length) return addBoard();
  $('boardPicker').showModal();
  pickRefresh();
}

// Grants a new port through Chrome's chooser and connects to it.
export async function addBoard() {
  closePicker();
  let p;
  try {
    p = await navigator.serial.requestPort();
  } catch (e) {
    if (e.name !== 'NotFoundError') toast(`Connect failed: ${e.message}`, 'err'); // NotFoundError = chooser cancelled
    return;
  }
  if (isBootPort(p)) {
    toast('That board is in its bootloader, waiting for firmware: flash it from Tools → Firmware update, or press its reset button once.', 'err');
    return;
  }
  await connectTo(p);
}

async function connectTo(p) {
  closePicker();
  await portLocks.get(p); // a picker probe may still hold it open
  try {
    await openPort(p);
  } catch (e) {
    // Usually another program (Arduino IDE serial monitor, arduino-cli upload, a script) holds the port.
    toast(`Connect failed: ${e.message}${e.name === 'NetworkError' || e.name === 'InvalidStateError' ? ' Is another program using the port?' : ''}`, 'err');
  }
}

// ---------- Board picker ----------
// Chrome's chooser names both boards after the USB driver ("Arduino MKR WAN 1310 (COMx)") and Web Serial
// doesn't expose the COM name, so for ports already granted the page asks each board for its role and
// lists them itself. The chooser is only needed to grant a new board (Add board…).
const PROBE_MS = 1500;
const ROLE_RANK = { house: 0, gate: 1, unset: 2 };
const portLocks = new WeakMap(); // port -> promise of the probe holding it open
let pickEntries = []; // { port, state: 'probing' | 'ok' | 'err', info, err, strobing }
let pickGen = 0; // bumped on every refresh/close so late probe results don't redraw a stale list
let pickSoonTimer = null;

async function knownPorts() {
  return (await navigator.serial.getPorts()).filter((p) => {
    const i = p.getInfo();
    return i.usbVendorId === USB_VID && i.usbProductId !== BOOT_PID;
  });
}

// Runs fn with the port to itself: probes of the same port queue up instead of failing as "in use".
function withPort(p, fn) {
  const run = (portLocks.get(p) || Promise.resolve()).then(fn);
  portLocks.set(p, run.catch(() => {}));
  return run;
}

// Opens the port just long enough to send one console command and read its reply.
function probePort(p, cmd = 'info') {
  return withPort(p, async () => {
    let timer;
    const timeout = new Promise((r) => { timer = setTimeout(() => r({ timedOut: true }), PROBE_MS); });
    await p.open({ baudRate: 115200 });
    let r, w;
    try {
      await p.setSignals({ dataTerminalReady: true, requestToSend: true }); // the console is silent without DTR
      r = p.readable.getReader();
      w = p.writable.getWriter();
      const id = nextId++;
      const sent = await Promise.race([w.write(new TextEncoder().encode(JSON.stringify({ id, cmd }) + '\n')), timeout]);
      if (sent?.timedOut) throw new Error('no answer');
      const dec = new TextDecoder();
      let buf = '';
      for (;;) {
        const { value, done, timedOut } = await Promise.race([r.read(), timeout]);
        if (timedOut) throw new Error('no answer');
        if (done) throw new Error('port closed');
        buf += dec.decode(value, { stream: true });
        let i;
        while ((i = buf.indexOf('\n')) >= 0) {
          const line = buf.slice(0, i).trim();
          buf = buf.slice(i + 1);
          let msg;
          try { msg = JSON.parse(line); } catch { continue; }
          if (msg.id === id) return msg;
        }
      }
    } finally {
      clearTimeout(timer);
      try { await r?.cancel(); } catch { /* the port may be gone */ }
      r?.releaseLock();
      w?.releaseLock();
      await p.close().catch(() => {});
    }
  });
}

export async function pickRefresh() {
  const gen = ++pickGen;
  const ports = await knownPorts();
  if (gen !== pickGen) return;
  if (!ports.length) return addBoard(); // the last board was unplugged or forgotten
  pickEntries = ports.map((port) => ({ port, state: 'probing' }));
  renderPicker();
  for (const e of pickEntries) {
    probePort(e.port).then((info) => {
      if (info.ok) Object.assign(e, { state: 'ok', info });
      else Object.assign(e, { state: 'err', err: new Error(info.error || 'no answer') });
    }, (err) => Object.assign(e, { state: 'err', err })).finally(() => { if (gen === pickGen) renderPicker(); });
  }
}

// A board plugged in or out while the picker is open; one that just enumerated may still be booting.
function pickRefreshSoon() {
  clearTimeout(pickSoonTimer);
  pickSoonTimer = setTimeout(() => { if ($('boardPicker').open) pickRefresh(); }, 1000);
}

export function closePicker() {
  pickGen++;
  clearTimeout(pickSoonTimer);
  if ($('boardPicker').open) $('boardPicker').close();
}

export const pickerClosed = () => pickGen++; // Escape

function pickText(e) {
  if (e.state === 'probing') return ['LoRa GateLink board', 'checking…'];
  if (e.state === 'err') {
    const busy = e.err.name === 'NetworkError' || e.err.name === 'InvalidStateError';
    return ['Arduino board', busy ? 'in use by another program'
      : e.err.message === 'no answer' ? 'no answer: still booting, or not GateLink firmware' : e.err.message];
  }
  const i = e.info;
  const sub = [`fw ${i.fw}`, i.key_set ? 'key set' : 'no key'];
  if (i.saved_role && i.saved_role !== i.role) sub.push(`${i.saved_role} after reboot`);
  return [`LoRa GateLink – ${cap(i.role)}`, sub.join(' · ')];
}

function renderPicker() {
  const rank = (e) => (e.state === 'ok' ? ROLE_RANK[e.info.role] ?? 3 : e.state === 'probing' ? 4 : 5);
  const focusKey = document.activeElement?.dataset.pick; // the list is redrawn; keep keyboard focus
  const list = $('pickList');
  list.innerHTML = '';
  const button = (key, text, title, onclick) => {
    const b = document.createElement('button');
    b.dataset.pick = key;
    b.textContent = text;
    b.title = title;
    b.onclick = onclick;
    return b;
  };
  pickEntries.map((e, k) => [e, k]).sort(([a], [b]) => rank(a) - rank(b)).forEach(([e, k]) => {
    const li = document.createElement('li');
    li.className = `pick-item ${e.state}`;
    const [title, sub] = pickText(e);
    const main = button(`${k}:main`, '', 'Connect to this board', () => connectTo(e.port));
    main.className = 'pick-main';
    main.innerHTML = `<b>${esc(title)}</b><small>${esc(sub)}</small>`;
    li.append(main);
    if (e.state === 'ok') {
      const id = button(`${k}:id`, e.strobing ? 'Strobing…' : 'Identify', 'Strobe this board\'s LED for 6 s', () => {
        e.strobing = true;
        renderPicker();
        probePort(e.port, 'identify').catch((err) => Object.assign(e, { state: 'err', err }));
        setTimeout(() => { e.strobing = false; renderPicker(); }, 6000);
      });
      id.disabled = !!e.strobing;
      li.append(id);
    }
    if (e.port.forget) {
      li.append(button(`${k}:forget`, 'Forget', 'Remove this page\'s access to the port (Add board… grants it again)', async () => {
        await portLocks.get(e.port);
        await e.port.forget().catch(() => {});
        pickRefresh();
      }));
    }
    list.append(li);
  });
  if (focusKey) list.querySelector(`[data-pick="${focusKey}"]`)?.focus();
}

async function openPort(p) {
  opening = true;
  try {
    await p.open({ baudRate: 115200 });
    try {
      await p.setSignals({ dataTerminalReady: true, requestToSend: true });
    } catch (e) {
      await p.close().catch(() => {});
      throw e;
    }
  } finally {
    opening = false;
  }
  const otherBoard = p !== lastPort; // its ping/diag results don't apply
  S.port = lastPort = p;
  const enc = new TextEncoderStream();
  writePipe = enc.readable.pipeTo(S.port.writable).catch(() => {});
  writer = enc.writable.getWriter();
  readLoop();
  await hooks.onOpen({ otherBoard, port: p });
}

// Without a lastPort (an update started from the bootloader) only the connect event can find the board.
export function startReconnect() {
  reconnectUntil = Date.now() + RECONNECT_MS;
  $('devline').textContent = 'reconnecting…';
  clearInterval(reconnectTimer);
  reconnectTimer = setInterval(() => tryReconnect(lastPort), 1500);
}

export function stopReconnect() {
  clearInterval(reconnectTimer);
  reconnectTimer = null;
  reconnectUntil = 0;
  expectBoard = null;
}

async function tryReconnect(p) {
  if (S.port || opening || !reconnectUntil) return;
  if (Date.now() > reconnectUntil) {
    stopReconnect();
    $('devline').textContent = 'not connected';
    toast('The board didn’t come back within 30 s. Click Connect board.', 'err');
    return;
  }
  if (!p) return;
  const was = expectBoard;
  try {
    await openPort(p);
  } catch {
    return; // not back yet; the timer retries
  }
  stopReconnect();
  logLine('reconnected');
  checkSameBoard(was);
}

// What the board that's going away should come back as. A reboot applies a role saved since (reboot_pending),
// and a firmware update (flashCheck reports it) changes fw.
function boardIdentity() {
  if (!S.boardInfo) return null;
  return { role: S.boardInfo.role, anyRole: S.rebootPending, fw: S.boardInfo.fw };
}

function checkSameBoard(was) {
  const now = S.boardInfo;
  if (!was || !now) return;
  const roleOk = was.anyRole || now.role === was.role;
  const fwOk = S.flashCheck || now.fw === was.fw;
  if (roleOk && fwOk) return;
  toast(`Reconnected to a different board: the ${now.role} board (fw ${now.fw}), not the ${was.role} board (fw ${was.fw}) `
    + 'that went away. Use Connect board to pick another.', 'err');
}

// Called when the board went away without the user asking (reset, reboot, cable).
async function connectionLost() {
  // The board that comes back on this port should be the same one; tryReconnect checks it.
  expectBoard = boardIdentity();
  await disconnect(true, true);
  startReconnect();
}

// Reboot the board and reconnect to it once it's back.
export async function rebootAndReconnect() {
  await request('reboot', {}, 1500).catch(() => {});
  const was = boardIdentity();
  await disconnect(true);
  startReconnect();
  expectBoard = was;
}

// The firmware update hands the port it reset into the bootloader: the board comes back there.
export const expectBack = (p) => { lastPort = p; };

// Waits for p, but no longer than ms: stream calls on a device that vanished can stay pending for good.
const within = (p, ms = 1000) => Promise.race([Promise.resolve(p).catch(() => {}), new Promise((r) => setTimeout(r, ms))]);

// lost: the device is gone, so pending writes are aborted rather than flushed.
export async function disconnect(quiet = false, lost = false) {
  if (!S.port) return;
  const p = S.port;
  S.port = null; // also stops readLoop from re-entering disconnect()
  hooks.onClosing();
  for (const req of pending.values()) req.reject(new Error('disconnected'));
  pending.clear();
  await within(reader?.cancel());
  await within(readPipe);
  await within(lost ? writer?.abort() : writer?.close());
  await within(writePipe);
  try {
    await p.close();
  } catch (e) {
    if (!quiet) logLine(`port close failed: ${e.message}`, 'err');
  }
  writer = reader = readPipe = writePipe = null;
  hooks.onClosed();
}

async function readLoop() {
  const dec = new TextDecoderStream();
  readPipe = S.port.readable.pipeTo(dec.writable).catch(() => {});
  reader = dec.readable.getReader();
  let buf = '';
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += value;
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i).trim();
        buf = buf.slice(i + 1);
        if (line) handleLine(line);
      }
    }
  } catch (e) {
    logLine(`serial read ended: ${e.message}`, 'err');
  }
  if (S.port) connectionLost();
}

// A line that throws while being handled (e.g. a status from other firmware missing a field) is logged and
// skipped. Thrown inside readLoop it ended the read loop, which dropped the connection and the buffered lines.
let lastLineError = '';
function handleLine(line) {
  try {
    onLine(line);
  } catch (e) {
    console.error(e, line);
    const msg = `couldn't handle a line from the board: ${e.message}`;
    if (msg !== lastLineError) logLine(`${msg} · ${line.slice(0, 120)}`, 'err'); // once, not on every poll
    lastLineError = msg;
  }
}

function onLine(line) {
  hooks.onRawLine(line);
  let msg;
  try { msg = JSON.parse(line); } catch { return; }
  if (msg.id !== undefined && pending.has(msg.id)) {
    const p = pending.get(msg.id);
    pending.delete(msg.id);
    clearTimeout(p.timer);
    p.resolve(msg);
    return;
  }
  if (msg.event) hooks.onEvent(msg);
}

export function request(cmd, args = {}, timeoutMs = 4000) {
  if (!writer) return Promise.reject(new Error('not connected'));
  const id = nextId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`${cmd}: timeout`));
    }, timeoutMs);
    pending.set(id, { resolve, reject, timer });
    // The write fails if the port drops mid-request; fail now rather than at the timeout.
    writer.write(JSON.stringify({ id, cmd, ...args }) + '\n').catch((e) => {
      clearTimeout(timer);
      pending.delete(id);
      reject(new Error(`${cmd}: ${e?.message || 'write failed'}`));
    });
  });
}

export async function call(cmd, args, timeoutMs) {
  const res = await request(cmd, args, timeoutMs);
  if (!res.ok) throw new Error(res.error || `${cmd} failed`);
  return res;
}

// USB plug events: refresh an open picker; a granted port reappearing while we wait for a reboot is the board coming
// back (the other board never left, so it can't fire this).
export function watchPorts() {
  navigator.serial?.addEventListener('disconnect', (e) => {
    if ($('boardPicker').open) pickRefreshSoon();
    if (e.target === S.port) connectionLost();
  });
  navigator.serial?.addEventListener('connect', (e) => {
    if ($('boardPicker').open) pickRefreshSoon();
    if (!reconnectUntil || S.port || isBootPort(e.target)) return;
    lastPort = e.target;
    tryReconnect(lastPort);
  });
}
