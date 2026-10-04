// GateLink web console: talks newline-delimited JSON to the board over Web Serial.
'use strict';

const $ = (id) => document.getElementById(id);
// Escape text for innerHTML (element content and quoted attributes).
const esc = (t) => String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

// ---------- Parameter presentation ----------
const GROUPS = [
  ['General', ['role', 'net_id']],
  ['Radio', ['freq_hz', 'sf', 'bw_hz', 'cr', 'tx_power', 'sync_word']],
  ['Link', ['retries', 'heartbeat_s', 'link_timeout_s', 'cmd_ttl_s']],
  ['Inputs', ['debounce_ms', 'in1_invert', 'in2_invert', 'in3_invert', 'in4_invert', 'power_sense']],
  ['Gate node', ['pulse_ms', 'travel_timeout_s']],
  ['House node', ['ctrl_sync', 'sync_window_ms', 'resync_ms', 'mismatch_timeout_s', 'sensor_invert', 'linkloss_open',
    'ctrl_power_sense', 'ctrl_confirm_ms', 'ctrl_settle_ms']],
  ['Board', ['uart_console']],
];
// Config tooltips: what the setting does, then when you'd change it.
const NO_INVERT = 'Leave off. Inverted, a dead opto or cut wire reads as active; fix a reversed signal in the wiring instead.';
const HELP = {
  role: 'Which end this board is: house (next to the controller) or gate (at the opener). Takes effect after Save and reboot.',
  net_id: 'Both boards must match. Change it only if another GateLink pair shares the channel nearby.',
  freq_hz: 'Both boards must match. US 902–928 MHz, EU 863–870 MHz. Move off the default only if Link history shows a high noise floor.',
  sf: 'Both boards must match. Raise it a step at a time if the link is weak or drops; each step roughly doubles airtime, so replies get slower. Lower it for faster replies on a strong link.',
  bw_hz: 'Both boards must match. Keep 500 kHz in the US (keeps a single-channel link within FCC rules). Narrower reaches further but every frame takes longer.',
  cr: 'Both boards must match. Extra error correction, 4/5 to 4/8. Raise it if Link history shows CRC errors with a decent signal; frames get longer.',
  tx_power: 'Transmit power in dBm; may differ between the boards. Lower it on USB power or at short range (full power can brown out a weak supply); raise it if the link is marginal.',
  sync_word: 'Both boards must match. Rarely changed: it filters out other LoRa networks on the same channel.',
  retries: 'Resends of an unacknowledged message, spread over its lifetime. Raise it on a lossy link; lower it to keep the channel quieter.',
  heartbeat_s: 'Gate: how often it reports status when nothing changes. Shorter spots a dead link sooner but uses more airtime. The house waits at least 2.5 heartbeats before calling the link lost.',
  link_timeout_s: 'House: silence from the gate this long = link lost (the contact sensor then reads open). Raise it if short dropouts cause false alarms; it is never shorter than 2.5 gate heartbeats.',
  cmd_ttl_s: 'House: how long a gate command keeps being retried before it is dropped (never fired late). Raise it if commands give up during short dropouts; lower it so a stale command isn’t delivered seconds later.',
  debounce_ms: 'How long an input must hold steady before it counts. Raise it if long field wires or a bouncy contact show up as flicker in the log.',
  in1_invert: `IN1 (house: controller output; gate: open limit). ${NO_INVERT}`,
  in2_invert: `IN2 (house: controller power; gate: closed limit). ${NO_INVERT}`,
  in3_invert: `IN3 (house: spare; gate: AC power). ${NO_INVERT}`,
  in4_invert: `IN4 (spare on both boards). ${NO_INVERT}`,
  power_sense: 'Gate: IN3 watches the AC-powered 24 V supply. Without AC, commands are refused; a limit that still reads (the opener runs on its battery) is trusted, and with none the gate reads “no power” instead of between. Turn off only if IN3 isn’t wired.',
  pulse_ms: 'Gate: how long the OPEN/CLOSE contact closes. Raise it if the opener misses short presses; keep it short, as other devices share these inputs.',
  travel_timeout_s: 'Longest a full open or close should take. Set it a little above your gate’s real travel time; past it, the gate counts as stuck.',
  ctrl_sync: 'House: K1 drives the controller’s switch input so the controller always shows the real gate state. Turn off only if the controller has no switch input.',
  sync_window_ms: 'House: after K1 changes, controller changes count as its echo, not a command, for this long. Raise it if a slow controller’s echo turns into an unwanted gate command.',
  resync_ms: 'House: how long K1 is released when re-syncing a controller that is out of step. Raise it if the controller doesn’t notice a short blip.',
  mismatch_timeout_s: 'House: how long the controller may disagree with the gate before K1 re-syncs it (at once after boot or power return). Lower it for faster correction; raise it if re-syncs fight someone using the controller.',
  sensor_invert: 'House: flips the contact sensor output (K2). Use it if the alarm shows open while the gate is closed.',
  linkloss_open: 'House: the contact sensor reads open while the link is lost, so the alarm never trusts a stale “closed”. Turn off only if dropouts cause too many false alerts.',
  ctrl_power_sense: 'House: IN2 watches the controller’s supply, so a power cut (which drops its relay) isn’t mistaken for a close command. Turn off only if IN2 isn’t wired.',
  ctrl_confirm_ms: 'House: each controller change waits this long before becoming a command, so one caused by a failing supply can be discarded. Raise it if power cuts still send commands; lower it for a snappier response.',
  uart_console: 'Also run this console on the board’s serial pins (13 RX, 14 TX; 3.3 V, 250 kbaud) for a USB-to-UART adapter. Bench power testing only: the adapter keeps its port while the board is unpowered. Leave off at the install.',
  ctrl_settle_ms: 'House: after the controller powers up (or the house boots), its changes count as sync for at least this long. Raise it if the controller takes longer to settle after power returns.',
};
// Settings that must be identical on both boards (marked * in the form). The link's retry and response timing
// assumes the peer's frames use the same air settings, so cr counts even though the LoRa header carries it.
// tx_power only sets how loud each board transmits and may differ.
const MUST_MATCH = new Set(['net_id', 'freq_hz', 'sf', 'bw_hz', 'cr', 'sync_word']);
const SELECTS = {
  role: [[0, 'unset'], [1, 'house'], [2, 'gate']],
  bw_hz: [[125000, '125 kHz'], [250000, '250 kHz'], [500000, '500 kHz']],
};
const IO_LABELS = {
  house: { in1: 'IN1 · Controller input', in2: 'IN2 · Controller power', in3: 'IN3 · spare', in4: 'IN4 · spare', k1: 'K1 · Controller sync', k2: 'K2 · Contact sensor' },
  gate: { in1: 'IN1 · Open limit', in2: 'IN2 · Closed limit', in3: 'IN3 · AC power', in4: 'IN4 · spare', k1: 'K1 · OPEN pulse', k2: 'K2 · CLOSE pulse' },
  unset: { in1: 'IN1', in2: 'IN2', in3: 'IN3', in4: 'IN4', k1: 'K1', k2: 'K2' },
};

// ---------- Field wiring (Install tab) ----------
// kind: out = a relay contact the board switches, in = a signal the board reads, pwr = power.
// rows: [board terminal, device terminal]. All board GND terminals are common.
// Inputs use the internal pull-down: active = driven to 3.3 V, unwired/open = off.
const V33 = '3.3 V (VCC)';
const SPARE_INPUTS = { name: 'Spare inputs (optional)', hint: 'Spare · e.g. beam break, alarm status', kind: 'in',
  rows: [['IN3 (A3)', 'Contact'], ['IN4 (A4)', 'Contact'], [V33, 'Common']] };
const SPARE_NOTE = 'IN3 (A3) and IN4 (A4) are spare inputs (contact to 3.3 V, internal pull-down), reserved for future use such as a beam-break sensor or alarm status. They are shown and logged but don’t affect behaviour yet. Leave spare inputs unwired if you don’t need them; they read “off”.';
const WIRING = {
  house: {
    groups: [
      { name: 'Controller relay output', hint: 'Dry contact · closed = open gate', kind: 'in',
        rows: [['IN1 (A1)', 'Contact'], [V33, 'Contact']] },
      { name: 'Controller power sense (opto, PNP)', hint: 'Channel across the controller’s 12 V supply', kind: 'in',
        rows: [['IN2 (A2)', 'OUT · controller power'], [V33, 'VCC (output side)'], ['GND', 'GND (output side)']] },
      { name: 'Controller switch input', hint: 'K1 closed while gate is not closed (held until travel ends)', kind: 'out',
        rows: [['K1 COM', 'Switch input'], ['K1 NO', 'Switch common']] },
      { name: 'Contact sensor', hint: 'K2 closed = gate closed', kind: 'out',
        rows: [['K2 COM', 'Terminal'], ['K2 NO', 'Terminal']] },
      SPARE_INPUTS,
      { name: '5 V supply', hint: 'Or USB', kind: 'pwr',
        rows: [['VIN (5 V)', '+5 V'], ['GND', '0 V']] },
    ],
    notes: [
      'IN1 reads the controller’s relay contact switched to the board’s 3.3 V (internal pull-down; open = off). The controller output must be a potential-free contact, and nothing above 3.3 V may reach IN1. If ON and OFF come out reversed, fix it in the wiring (use the other relay contact, or change the controller’s output mode), never with <code>in1_invert</code>: inverted, a cut wire would read as ON. Keep all <code>inN_invert</code> at 0.',
      'K1 mirrors the real gate back to the controller so its switch always shows the true state. Set the controller’s switch input to toggle/follow mode (contact closed = ON, open = OFF), not detached. Wire it per the controller’s switch-input diagram. Low voltage only; never switch mains with the shield.',
      'IN2 senses the controller’s supply through a PNP-output opto channel wired across it (use a channel rated for that voltage; output side from 3.3 V only). When the controller loses power its relay drops, which would otherwise look like a user turning the switch off: while IN2 is off, controller edges are logged but never sent, each edge waits <code>ctrl_confirm_ms</code> in case power is failing, and after power returns its edges count as sync for at least <code>ctrl_settle_ms</code>. Set <code>ctrl_power_sense</code> to 0 if IN2 isn’t wired.',
      'The contact sensor needs an external terminal input. K2 closes when the gate is closed and opens if the link is lost (<code>linkloss_open</code>). <code>sensor_invert</code> flips it.',
      'VIN is 5 V max. USB power is fine for the house board.',
      SPARE_NOTE,
    ],
  },
  gate: {
    groups: [
      { name: 'Opener OPEN input', hint: 'Pulsed only · shared with other devices', kind: 'out',
        rows: [['K1 NO', 'OPEN'], ['K1 COM', 'COM']] },
      { name: 'Opener CLOSE input', hint: 'Pulsed only · shared with other devices', kind: 'out',
        rows: [['K2 NO', 'CLOSE'], ['K2 COM', 'COM']] },
      { name: 'Opto board outputs (PNP)', hint: 'Output side powered from 3.3 V only', kind: 'in',
        rows: [['IN1 (A1)', 'OUT1 · open limit'], ['IN2 (A2)', 'OUT2 · closed limit'], ['IN3 (A3)', 'OUT3 · AC 24 V supply'],
          ['IN4 (A4)', 'OUT4 · spare'], [V33, 'VCC (output side)'], ['GND', 'GND (output side)']] },
      { name: '24 V → 5 V buck', hint: 'Fed from the opener’s 24 V accessory output or the AC 24 V supply', kind: 'pwr',
        rows: [['VIN (5 V)', '+5 V out'], ['GND', '0 V out']] },
    ],
    notes: [
      'K1/K2 go in parallel with whatever else is already on the opener’s OPEN/CLOSE inputs. GateLink only pulses them (<code>pulse_ms</code>) and never holds them, so the other devices keep working.',
      'All gate inputs go through a PNP-output opto board. Power its output side from the board’s 3.3 V, never 5 V or 24 V: a PNP output passes that voltage straight to the input pin. A lit opto drives its input to 3.3 V (active); a dead opto, missing 24 V or cut wire reads off.',
      'Opto input side (24 V): wet each limit contact from the opener’s 24 V accessory output — 24 V to AUX C; the open-limit relay’s NO to channel 1, and the closed-limit relay’s <strong>NC</strong> to channel 2 (it energizes when not at the close limit) — and connect the third channel across the AC-powered 24 V supply (not the battery-backed accessory output). Follow the opto board’s input markings for the common. Set AUX relay A to <em>open limit</em> and AUX relay B to <em>closed limit</em> in the opener’s menu.',
      'Gate state comes only from the limit inputs. IN3 senses AC power: without it commands are refused. The opener runs on its battery, so a limit that still reads is trusted; with none reading the gate shows “no power” (it may be moving, or the opener may be dead) rather than between. Set <code>power_sense</code> to 0 if IN3 isn’t wired.',
      'VIN is 5 V max. Never connect the opener’s 24 V directly to the board.',
      'Use the relays’ NO/COM contacts only. Add TVS or RC suppression on long field runs.',
      'Keep the antenna vertical and outside any metal enclosure.',
      'IN4 (A4) is spare, reserved for future use. It is shown and logged but doesn’t affect behaviour yet.',
    ],
  },
};

let wiringShown = 'house';

// Greedy word wrap for SVG text (no auto-wrap there); `max` is in characters.
function wrapText(text, max) {
  const lines = [];
  let line = '';
  for (const w of text.split(' ')) {
    if (line && line.length + 1 + w.length > max) {
      lines.push(line);
      line = w;
    } else line = line ? `${line} ${w}` : w;
  }
  return line ? [...lines, line] : lines;
}

// Two layouts: side by side, or a compact one for phones that fits ~360 px without scrolling (text wraps).
const WIRING_LAYOUT = {
  wide: { bx: 16, bw: 250, dx: 500, dw: 250, width: 766, rowH: 28, title: 30, hint: 38, sub: 40 },
  narrow: { bx: 4, bw: 118, dx: 150, dw: 206, width: 360, rowH: 26, title: 23, hint: 30, sub: 16 },
};
let wiringNarrow = false;

function renderWiring(r) {
  wiringShown = r;
  const W = WIRING[r];
  document.querySelectorAll('[data-wiring]').forEach((b) => b.classList.toggle('active', b.dataset.wiring === r));

  const L = WIRING_LAYOUT[wiringNarrow ? 'narrow' : 'wide'];
  const { bx, bw, dx, dw, width, rowH } = L;
  const gap = 14;
  const sub = wrapText('MKR WAN 1310 + Relay Proto Shield', L.sub);
  const top = 54 + sub.length * 15;
  let y = top;
  let blocks = '', wires = '';
  for (const g of W.groups) {
    let ty = y + 19, head = '';
    for (const t of wrapText(g.name, L.title)) { head += `<text class="title" x="${dx + 12}" y="${ty}">${esc(t)}</text>`; ty += 17; }
    ty -= 2;
    for (const t of wrapText(g.hint, L.hint)) { head += `<text class="dim" x="${dx + 12}" y="${ty}">${esc(t)}</text>`; ty += 14; }
    const headH = ty - 14 - y + 8;
    const h = headH + g.rows.length * rowH;
    blocks += `<rect class="blk" x="${dx}" y="${y}" width="${dw}" height="${h}" rx="8"/>${head}`;
    g.rows.forEach(([bt, dt], i) => {
      const cy = y + headH + i * rowH + rowH / 2;
      wires += `<line class="w-${g.kind}" x1="${bx + bw}" y1="${cy}" x2="${dx}" y2="${cy}"/>`
        + `<circle class="w-${g.kind}" cx="${bx + bw}" cy="${cy}" r="4.5"/>`
        + `<circle class="w-${g.kind}" cx="${dx}" cy="${cy}" r="4.5"/>`
        + `<text class="term" x="${bx + bw - 12}" y="${cy + 4}" text-anchor="end">${esc(bt)}</text>`
        + `<text class="term" x="${dx + 12}" y="${cy + 4}">${esc(dt)}</text>`;
    });
    y += h + gap;
  }
  const height = y - gap + 16;
  const board = `<rect class="blk board" x="${bx}" y="16" width="${bw}" height="${height - 32}" rx="8"/>`
    + `<text class="title" x="${bx + 12}" y="38">${r === 'house' ? 'House' : 'Gate'} board</text>`
    + sub.map((t, i) => `<text class="dim" x="${bx + 12}" y="${54 + i * 15}">${esc(t)}</text>`).join('');
  const svg = $('wiringSvg');
  svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
  svg.innerHTML = board + blocks + wires;

  $('wiringTable').innerHTML = '<thead><tr><th>Device</th><th>Board terminal</th><th>Device terminal</th></tr></thead><tbody>'
    + W.groups.map((g) => g.rows.map(([bt, dt], i) =>
      `<tr>${i === 0 ? `<td class="dev" rowspan="${g.rows.length}">${esc(g.name)}<small>${esc(g.hint)}</small></td>` : ''}`
      + `<td class="mono">${esc(bt)}</td><td class="mono">${esc(dt)}</td></tr>`).join('')).join('')
    + '</tbody>';
  $('wiringNotes').innerHTML = W.notes.map((n) => `<li>${n}</li>`).join('');
}

// ---------- Serial transport ----------
let port = null;
let writer = null;
let reader = null;
let nextId = 1;
const pending = new Map();
let pollTimer = null;
let pingTimer = null;

let role = 'unset';
let keySet = false;
// Settings applied to the running config since the last Save (this page's own record; lost on reboot).
let appliedUnsaved = false;
let meta = [];
let params = {};
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

async function connect() {
  stopReconnect();
  if (!(await knownPorts()).length) return addBoard();
  $('boardPicker').showModal();
  pickRefresh();
}

// Grants a new port through Chrome's chooser and connects to it.
async function addBoard() {
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
    let reader, writer;
    try {
      await p.setSignals({ dataTerminalReady: true, requestToSend: true }); // the console is silent without DTR
      reader = p.readable.getReader();
      writer = p.writable.getWriter();
      const id = nextId++;
      const sent = await Promise.race([writer.write(new TextEncoder().encode(JSON.stringify({ id, cmd }) + '\n')), timeout]);
      if (sent?.timedOut) throw new Error('no answer');
      const dec = new TextDecoder();
      let buf = '';
      for (;;) {
        const { value, done, timedOut } = await Promise.race([reader.read(), timeout]);
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
      try { await reader?.cancel(); } catch {}
      reader?.releaseLock();
      writer?.releaseLock();
      await p.close().catch(() => {});
    }
  });
}

async function pickRefresh() {
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

function closePicker() {
  pickGen++;
  clearTimeout(pickSoonTimer);
  if ($('boardPicker').open) $('boardPicker').close();
}

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
  if (p !== lastPort) resetToolsView(); // another board: its ping/diag results don't apply
  port = lastPort = p;
  const enc = new TextEncoderStream();
  writePipe = enc.readable.pipeTo(port.writable).catch(() => {});
  writer = enc.writable.getWriter();
  readLoop();
  setConnected(true);
  try {
    await refreshInfo();
    await loadConfig();
    await refreshStatus();
  } catch (e) {
    toast(`Connected, but the board didn't answer: ${e.message}`, 'err');
  }
  if (port !== p) return; // dropped while querying: disconnect() already ran; don't leave an orphan poll
  clearInterval(pollTimer);
  pollTimer = setInterval(pollStatus, POLL_MS);
}

// Without a lastPort (an update started from the bootloader) only the connect event can find the board.
function startReconnect() {
  reconnectUntil = Date.now() + RECONNECT_MS;
  $('devline').textContent = 'reconnecting…';
  clearInterval(reconnectTimer);
  reconnectTimer = setInterval(() => tryReconnect(lastPort), 1500);
}

function stopReconnect() {
  clearInterval(reconnectTimer);
  reconnectTimer = null;
  reconnectUntil = 0;
}

async function tryReconnect(p) {
  if (port || opening || !reconnectUntil) return;
  if (Date.now() > reconnectUntil) {
    stopReconnect();
    $('devline').textContent = 'not connected';
    toast('The board didn’t come back within 30 s. Click Connect board.', 'err');
    return;
  }
  if (!p) return;
  try {
    await openPort(p);
    stopReconnect();
    logLine('reconnected');
  } catch {} // not back yet; the timer retries
}

// Called when the board went away without the user asking (reset, reboot, cable).
async function connectionLost() {
  await disconnect(true);
  startReconnect();
}

async function disconnect(quiet = false) {
  if (!port) return;
  const p = port;
  port = null; // also stops readLoop from re-entering disconnect()
  clearInterval(pollTimer);
  clearInterval(pingTimer);
  clearTimeout(diagTimer);
  clearTimeout(pingWait);
  pingWait = null;
  $('pingAuto').checked = false;
  for (const req of pending.values()) req.reject(new Error('disconnected'));
  pending.clear();
  try { await reader?.cancel(); } catch {}
  await readPipe;
  try { await writer?.close(); } catch {}
  await writePipe;
  try {
    await p.close();
  } catch (e) {
    if (!quiet) logLine(`port close failed: ${e.message}`, 'err');
  }
  writer = reader = readPipe = writePipe = null;
  setConnected(false);
}

async function readLoop() {
  const dec = new TextDecoderStream();
  readPipe = port.readable.pipeTo(dec.writable).catch(() => {});
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
        if (line) onLine(line);
      }
    }
  } catch (e) {
    logLine(`serial read ended: ${e.message}`, 'err');
  }
  if (port) connectionLost();
}

function onLine(line) {
  if ($('logRaw').checked) logLine(line, 'raw');
  let msg;
  try { msg = JSON.parse(line); } catch { return; }
  if (msg.id !== undefined && pending.has(msg.id)) {
    const p = pending.get(msg.id);
    pending.delete(msg.id);
    clearTimeout(p.timer);
    p.resolve(msg);
    return;
  }
  if (msg.event) onEvent(msg);
}

function request(cmd, args = {}, timeoutMs = 4000) {
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

async function call(cmd, args, timeoutMs) {
  const res = await request(cmd, args, timeoutMs);
  if (!res.ok) throw new Error(res.error || `${cmd} failed`);
  return res;
}

// ---------- UI state ----------
// Controls that work without a board.
const OFFLINE_OK = ['logRaw', 'btnLogClear', 'btnLogSave', 'keyInput', 'btnKeyGen', 'btnKeyCopy', 'btnFwLatest', 'fwFile', 'btnBootPort'];

function setConnected(on) {
  $('btnConnect').hidden = on;
  $('btnDisconnect').hidden = !on;
  $('btnIdentify').hidden = !on;
  document.querySelectorAll('main button, main input, main select').forEach((el) => {
    if (el.closest('#tab-install') || el.classList.contains('info')) return; // reference, usable without a board
    if (!OFFLINE_OK.includes(el.id)) el.disabled = !on;
  });
  document.body.classList.toggle('offline', !on);
  pollMisses = 0;
  if (!on) markUnsaved(false); // a reboot drops them; another board never had them
  if (!on) clearHistoryView(); // another board (or this one after a reset) has a different record
  $('btnHistCsv').disabled = !hist?.buckets.length;
  if (!on) {
    boardInfo = null;
    updateFwCard();
    $('devline').textContent = 'not connected';
    $('keyWarn').hidden = true;
    document.title = 'GateLink Console';
  }
}

const cap = (t) => t[0].toUpperCase() + t.slice(1);

// Tab title: the gate state first, so a background tab still shows it.
function updateTitle(gate) {
  const parts = [gate, role !== 'unset' && cap(role), 'GateLink'].filter(Boolean);
  document.title = parts.length > 1 ? parts.join(' · ') : 'GateLink Console';
}

function applyRole(r) {
  role = r;
  updateTitle();
  document.querySelectorAll('[data-role]').forEach((el) => { el.hidden = el.dataset.role !== role; });
  const hint = {
    gate: 'K1 pulses the opener OPEN input and K2 the CLOSE input — this moves the real gate.',
    house: 'K1 toggles the controller sync output (gate commands are paused during the test). K2 drives the contact sensor output.',
    unset: 'Set a role first.',
  };
  $('relayHint').textContent = hint[role] || '';
  $('btnK1').textContent = role === 'gate' ? 'Pulse K1 (OPEN)' : 'Pulse K1';
  $('btnK2').textContent = role === 'gate' ? 'Pulse K2 (CLOSE)' : 'Pulse K2';
  if (WIRING[role]) renderWiring(role);
}

async function refreshInfo() {
  const info = await call('info');
  boardInfo = info;
  updateFwCard();
  if (flashCheck) reportFlash(info);
  devline = `${info.board} · fw ${info.fw} · ${info.role}`;
  $('devline').textContent = devline;
  keySet = !!info.key_set;
  $('keyWarn').hidden = info.key_set;
  $('secKeySet').textContent = info.key_set ? 'yes' : 'no (link disabled)';
  // Boards before 0.5.0 don't report it: program flash, erased by uploads.
  const spi = info.cfg_store === 'spi';
  $('secStore').textContent = spi ? 'flash chip (kept across firmware updates)' : 'program flash (erased by firmware updates)';
  $('secStore').className = spi ? '' : 'bad';
  applyRole(info.role);
}

function fmtDur(ms) {
  if (ms < 0) return 'never';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ${s % 60}s`;
  const h = Math.floor(m / 60);
  if (h < 48) return `${h}h ${m % 60}m`;
  return `${Math.floor(h / 24)}d ${h % 24}h`;
}

const yesNo = (v) => (v ? 'yes' : 'no');
// Frequency error in Hz, signed, with the crystal offset it means at our carrier (ppm).
function fmtFei(hz) {
  if (hz === undefined || hz === null) return '—';
  const ppm = params.freq_hz ? ` (${(Math.abs(hz) / (params.freq_hz / 1e6)).toFixed(2)} ppm)` : '';
  return `${hz > 0 ? '+' : ''}${Math.round(hz)} Hz${ppm}`;
}
const pill = (on) => `<span class="pill ${on ? 'on' : ''}">${on ? 'ON' : 'off'}</span>`;

async function refreshStatus(timeoutMs) {
  const res = await call('status', {}, timeoutMs);
  renderStatus(res.status);
}

// Status poll. Two misses in a row (the port is open but the board doesn't answer) dim the status cards
// like a disconnect, and the header says so, so stale values don't read as live.
let pollMisses = 0;
let devline = '';
let boardInfo = null; // last info reply
// The board answers status at once; a timeout under the poll interval keeps polls from piling up.
const POLL_MS = 2000;
let polling = false;
async function pollStatus() {
  if (polling) return;
  polling = true;
  try {
    await refreshStatus(POLL_MS - 100);
    pollMisses = 0;
  } catch {
    pollMisses++;
  } finally {
    polling = false;
  }
  if (!port) return;
  const stale = pollMisses >= 2;
  document.body.classList.toggle('offline', stale);
  $('devline').textContent = stale ? `${devline} · not responding` : devline;
}

function renderStatus(s) {
  if (s.role !== role) applyRole(s.role);
  const gs = s.gate || 'unknown';
  const g = $('gateState');
  g.textContent = s.role === 'unset' ? 'role not set' : gs.replaceAll('_', ' ');
  g.className = `gate-state ${gs}`;
  updateTitle(s.role === 'unset' ? '' : cap(gs.replaceAll('_', ' ')));
  $('gateCause').textContent = s.cause ?? '—';
  $('gateResult').textContent = s.last_result ?? '—';
  $('gateTarget').textContent = s.target || '—';

  const l = s.link;
  $('lnkVerified').innerHTML = l.verified ? '<span class="good">yes</span>' : '<span class="bad">no</span>';
  $('lnkAge').textContent = l.age_ms < 0 ? 'never' : `${fmtDur(l.age_ms)} ago`;
  $('lnkRssi').textContent = l.age_ms < 0 ? '—' : `${l.rssi} dBm / ${Number(l.snr).toFixed(1)} dB`;
  $('lnkTxRx').textContent = `${l.tx} / ${l.rx}`;
  $('lnkRetry').textContent = `${l.retries} / ${l.giveups}`;
  $('lnkBad').textContent = `${l.mac_fail} / ${l.replay}`;
  $('lnkLbt').textContent = l.lbt_defers === undefined ? '—' : `${l.lbt_defers} / ${l.lbt_forced}`;
  // Newer fields: absent on older firmware; noise is null before its first sample, fei before the first frame.
  $('lnkCrc').textContent = l.crc_err ?? '—';
  $('lnkNoise').textContent = l.noise === undefined || l.noise === null ? '—' : `${fmtNum(l.noise)} dBm`;
  $('lnkFei').textContent = fmtFei(l.age_ms < 0 ? null : l.fei);

  const labels = IO_LABELS[s.role] || IO_LABELS.unset;
  $('ioList').innerHTML = ['in1', 'in2', 'in3', 'in4', 'k1', 'k2'].filter((k) => k in s.io)
    .map((k) => `<div class="kv"><span>${labels[k]}</span>${pill(s.io[k])}</div>`).join('');

  $('bRole').textContent = s.reboot_pending ? `${s.role} (reboot to apply saved role)` : s.role;
  $('bFw').textContent = s.fw;
  $('bUp').textContent = fmtDur(s.uptime_ms);
  $('bReset').textContent = (s.reset_cause ?? '—').replaceAll('_', ' ');
  const nf = Number(s.radio_faults) || 0;
  const faults = nf ? ` <span class="bad">· ${nf} TX fault${nf === 1 ? '' : 's'}</span>` : '';
  $('bRadio').innerHTML = (s.radio_ok ? '<span class="good">ok</span>' : '<span class="bad">not initialised</span>') + faults;
  $('keyWarn').hidden = s.key_set;

  if (s.role === 'house') {
    const r = s.remote || {};
    $('lnkRemoteRssi').textContent = r.uptime_s ? `${r.rssi} dBm / ${Number(r.snr).toFixed(1)} dB` : '—';
    $('hCtrl').innerHTML = pill(s.ctrl);
    $('hCtrlPower').innerHTML = 'ctrl_power' in s ? (s.ctrl_power ? pill(true) : '<span class="bad">off · edges ignored</span>') : '—';
    $('hArmed').textContent = yesNo(s.armed);
    $('hSync').textContent = `${yesNo(s.sync_window)} / ${yesNo(s.resyncing)}`;
    const cmdRes = { '-1': 'none', '-2': 'gave up', 0: 'ok', 1: 'already there', 2: 'rejected', 4: 'refused: no AC power' };
    $('hCmd').textContent = `#${s.cmd_id} · ${s.cmd_pending ? 'sending…' : cmdRes[s.cmd_result] ?? s.cmd_result}`;
    $('hLimits').textContent = r.uptime_s ? `open ${r.open_limit ? '●' : '○'}  closed ${r.close_limit ? '●' : '○'}` : '—';
    $('hSpare').textContent = r.uptime_s && 'in3' in r ? `AC ${r.ac_power ?? r.in3 ? '●' : '○'}  IN4 ${r.in4 ? '●' : '○'}` : '—';
    $('hGateUp').textContent = r.uptime_s ? fmtDur(r.uptime_s * 1000) : '—';
    // The house waits max(link_timeout_s, 2.5 × the gate's heartbeat) before declaring the link down.
    $('hTiming').textContent = `${r.heartbeat_s ? `${r.heartbeat_s} s` : '—'} / ${s.link_timeout_eff_s ? `${s.link_timeout_eff_s} s` : '—'}`;
  }
}

// ---------- Config ----------
async function loadConfig() {
  const res = await call('config.get');
  meta = res.meta;
  params = res.params;
  renderConfig();
  // Remote-writable params come from the board's meta, so firmware changes to P_REMOTE follow automatically.
  const sel = $('remParam');
  const keep = sel.value;
  sel.replaceChildren(...meta.filter((m) => m.remote).map((m) => new Option(m.name, m.name)));
  if (keep && meta.some((m) => m.remote && m.name === keep)) sel.value = keep;
  syncRemValue();
}

// Show the selected remote param's range on the value box (the board still validates it).
function syncRemValue() {
  const m = meta.find((x) => x.name === $('remParam').value);
  const el = $('remValue');
  if (!m) return;
  el.min = m.min;
  el.max = m.max;
  el.placeholder = `${m.min}–${m.max}`;
}

function renderConfig() {
  const byName = Object.fromEntries(meta.map((m) => [m.name, m]));
  const form = $('cfgForm');
  form.innerHTML = '';
  for (const [title, names] of GROUPS) {
    const card = document.createElement('div');
    card.className = 'card';
    card.innerHTML = `<div class="label">${title}</div>`;
    for (const name of names) {
      const m = byName[name];
      if (!m) continue;
      const row = document.createElement('div');
      row.className = 'field';
      const id = `p_${name}`;
      let input;
      if (!SELECTS[name] && m.min === 0 && m.max === 1) {
        input = `<input id="${id}" type="checkbox" class="toggle" role="switch">`;
      } else if (SELECTS[name]) {
        const opts = SELECTS[name];
        input = `<select id="${id}">${opts.map(([v, t]) => `<option value="${v}">${t}</option>`).join('')}</select>`;
      } else {
        input = `<input id="${id}" type="number" min="${esc(m.min)}" max="${esc(m.max)}" step="1">`;
      }
      const help = HELP[name]
        ? `<button type="button" class="info" aria-label="About ${esc(name)}" aria-describedby="${id}_tip">i</button>`
          + `<span class="tip" role="tooltip" id="${id}_tip">${esc(HELP[name])}</span>`
        : '';
      // Info icon in its own column at the far right, after the entry field (an empty cell keeps rows aligned).
      const star = MUST_MATCH.has(name) ? '<span class="must" title="Must match on both boards" aria-label="must match on both boards">*</span>' : '';
      row.innerHTML = `<label for="${id}">${esc(name)}${star}</label>${input}${help || '<span></span>'}`;
      card.appendChild(row);
      const el = row.querySelector('input, select');
      setField(el, params[name]);
      el.addEventListener('input', () => {
        const v = fieldValue(el);
        row.classList.toggle('dirty', v !== params[name]);
        if (el.type === 'number') el.setAttribute('aria-invalid', String(!Number.isInteger(v) || v < m.min || v > m.max || el.value === ''));
        updateDirtyCount();
      });
      if (el.type === 'number') el.addEventListener('keydown', (e) => { if (e.key === 'Enter') $('btnCfgApply').click(); });
    }
    form.appendChild(card);
  }
  updateDirtyCount();
}

// Edits in the form not yet applied to the board.
const dirtyCount = () => Object.keys(formValues(true)).length;
function updateDirtyCount() {
  const n = dirtyCount();
  $('btnCfgApply').textContent = n ? `Apply (${n})` : 'Apply';
  markConfigTab();
}

// A dot on the Config tab while edits are unapplied or applied settings are unsaved, seen from any tab.
function markConfigTab() {
  const t = $('tabbtn-config');
  const n = meta.length ? dirtyCount() : 0;
  const pending = n > 0 || appliedUnsaved;
  t.classList.toggle('pending', pending);
  t.title = n ? `${n} unapplied edit${n === 1 ? '' : 's'}` : appliedUnsaved ? 'Applied settings not saved to flash yet' : '';
}

function markUnsaved(on) {
  appliedUnsaved = on;
  const b = $('btnCfgSave');
  b.textContent = on ? 'Save to flash •' : 'Save to flash';
  b.title = on ? 'Applied settings are running but not saved yet: a reboot or power cut loses them' : '';
  markConfigTab();
}

// On/off parameters are checkbox toggles; everything else carries its value in .value.
function fieldValue(el) {
  return el.type === 'checkbox' ? Number(el.checked) : Number(el.value);
}

function setField(el, v) {
  if (el.type === 'checkbox') el.checked = !!v;
  else el.value = v;
}

function formValues(onlyDirty) {
  const out = {};
  for (const m of meta) {
    const el = $(`p_${m.name}`);
    if (!el) continue;
    const v = fieldValue(el);
    if (!onlyDirty || v !== params[m.name]) out[m.name] = v;
  }
  return out;
}

const CFG_CHUNK = 8;

async function applyConfig() {
  const changes = formValues(true);
  if (!Object.keys(changes).length) return toast('No changes.');
  for (const [k, v] of Object.entries(changes)) {
    const m = meta.find((x) => x.name === k);
    if (!Number.isInteger(v) || v < m.min || v > m.max) return toast(`${k} must be an integer ${m.min}–${m.max}`, 'err');
  }
  // Send at most CFG_CHUNK params per request so a full import stays well inside the board's line buffer.
  const entries = Object.entries(changes);
  const applied = [], errors = [];
  let reboot = false;
  for (let i = 0; i < entries.length; i += CFG_CHUNK) {
    const chunk = Object.fromEntries(entries.slice(i, i + CFG_CHUNK));
    const res = await request('config.set', { params: chunk });
    applied.push(...(res.applied || []));
    errors.push(...(res.errors || []));
    // A request-level error (no per-param list) rejects the whole chunk.
    if (!res.ok && !res.errors?.length) errors.push(...Object.keys(chunk).map((k) => `${k} (${res.error || 'failed'})`));
    reboot ||= !!res.reboot_required;
  }
  await loadConfig();
  if (applied.length) markUnsaved(true);
  const done = applied.length ? `Applied ${applied.join(', ')}. ${reboot ? 'Save and reboot for role change.' : 'Remember to Save.'}` : '';
  if (errors.length) toast(`Rejected: ${errors.join(', ')}. ${done}`, 'err');
  else toast(done || 'Nothing changed.');
}

function download(name, text, type = 'application/json') {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

async function importConfig(file) {
  let data;
  try { data = JSON.parse(await file.text()); } catch { return toast('Not a valid JSON file.', 'err'); }
  const src = data?.params || data;
  if (!src || typeof src !== 'object') return toast('No GateLink settings found in that file.', 'err');
  let found = 0;
  for (const m of meta) {
    const el = $(`p_${m.name}`);
    if (el && Number.isInteger(src[m.name])) {
      setField(el, src[m.name]);
      el.dispatchEvent(new Event('input'));
      found++;
    }
  }
  if (!found) return toast('No GateLink settings found in that file.', 'err');
  const n = dirtyCount();
  toast(`Imported ${found} setting${found === 1 ? '' : 's'}; ${n ? `${n} differ${n === 1 ? 's' : ''} from the board. Review, then Apply and Save.` : 'all match the board already.'}`);
}

// ---------- Tools ----------
const rssiHist = [];
// DIAG over LoRa is best effort; give up on the reply after this long.
const DIAG_TIMEOUT_MS = 10000;
let diagTimer = null;
// A ping is one unacknowledged frame each way; at SF12 the round trip takes a few seconds.
const PING_TIMEOUT_MS = 8000;
let pingWait = null;

async function ping() {
  await call('radio.ping');
  if (pingWait) return; // timed from the oldest unanswered ping, or auto-ping would never time out
  if (!$('pingAuto').checked) $('pingRtt').textContent = 'waiting…';
  pingWait = setTimeout(() => {
    pingWait = null;
    $('pingRtt').textContent = 'no reply';
  }, PING_TIMEOUT_MS);
}

function drawRssi() {
  const svg = $('rssiChart');
  if (!rssiHist.length) { svg.innerHTML = ''; $('rssiRange').textContent = ''; return; }
  const all = rssiHist.flatMap((p) => [p.here, p.peer]);
  const lo = Math.min(...all) - 3, hi = Math.max(...all) + 3;
  const x = (i) => (rssiHist.length === 1 ? 150 : (i / (rssiHist.length - 1)) * 300);
  const y = (v) => 85 - ((v - lo) / (hi - lo || 1)) * 80;
  const line = (k, color) =>
    `<polyline stroke="var(${color})" points="${rssiHist.map((p, i) => `${x(i)},${y(p[k])}`).join(' ')}"/>`;
  svg.innerHTML = line('here', '--chart-here') + line('peer', '--chart-peer');
  $('rssiRange').textContent = `${Math.round(lo + 3)} … ${Math.round(hi - 3)} dBm`;
}

// Results that belong to one board.
function resetToolsView() {
  rssiHist.length = 0;
  drawRssi();
  for (const id of ['pingRtt', 'pingHere', 'pingPeer']) $(id).textContent = '—';
  $('diagOut').textContent = '—';
  $('remResult').textContent = '';
}

// ---------- Link history (Tools tab) ----------
// The board's hourly link record (hist.get, docs/console.md#link-history), fetched page by page. Buckets count
// from the board's boot: bucket i started now_s − i × period_s seconds before the reply.
const SNR_FLOOR = { 7: -7.5, 8: -10, 9: -12.5, 10: -15, 11: -17.5, 12: -20 }; // SX127x demodulation limit, dB
let hist = null; // { buckets, period, nowS, current, fetchedAt, peer, sf, role }
let histAt = -1; // bucket (index into hist.buckets) under the crosshair, -1 = none
let histLayout = null;

async function loadHistory() {
  const buckets = [];
  let from = null, res;
  $('histPlot').classList.add('loading'); // keep the old chart while the pages come in
  try {
    for (;;) {
      res = await request('hist.get', from === null ? {} : { from });
      if (!res.ok) {
        throw new Error(res.error === 'unknown cmd' ? 'This firmware has no link history (0.4.0 or later needed).' : res.error || 'hist.get failed');
      }
      buckets.push(...res.rows.map((r) => Object.fromEntries(res.fields.map((f, i) => [f, r[i]]))));
      // A bucket closing while we page moves `current` on: keep going until the last page reaches it.
      if (!res.rows.length || res.rows[res.rows.length - 1][0] >= res.current) break;
      from = res.rows[res.rows.length - 1][0] + 1;
    }
  } finally {
    $('histPlot').classList.remove('loading');
  }
  hist = {
    buckets, period: res.period_s, nowS: res.now_s, current: res.current, fetchedAt: Date.now(),
    peer: role === 'house', sf: params.sf, role,
  };
  histAt = -1;
  renderHistory();
}

const HIST_NOTE = $('histNote').textContent;

function clearHistoryView() {
  hist = null;
  histAt = -1;
  histLayout = null;
  $('histBody').hidden = true;
  $('btnHistCsv').disabled = true;
  $('btnHistLoad').textContent = 'Load history';
  $('histNote').textContent = HIST_NOTE;
}

const histStart = (b) => hist.fetchedAt - (hist.nowS - b.idx * hist.period) * 1000;
// Seconds the bucket covers: all of its period, or up to now for the one in progress.
const histSpan = (b) => (b.idx < hist.current ? hist.period : Math.max(1, hist.nowS - b.idx * hist.period));
const fmtClock = (ms) => new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
const fmtDay = (ms) => new Date(ms).toLocaleDateString([], { weekday: 'short' });
const fmtNum = (v, d = 1) => (v === null || v === undefined ? '—' : Number(v).toFixed(d).replace(/\.0+$/, ''));
const sum = (arr, f) => arr.reduce((a, b) => a + (f(b) || 0), 0);

// Buckets are hours unless hist.clear set another period (the e2e suite uses 60 s).
const histPeriodText = () => (hist.period % 60 ? `${hist.period} s` : `${hist.period / 60} min`);
const histBuckets = (n) => (hist.period === 3600 ? `${n} hour${n === 1 ? '' : 's'}` : `${n} bucket${n === 1 ? '' : 's'}`);

function renderHistory() {
  const B = hist.buckets;
  $('histBody').hidden = !B.length;
  $('btnHistCsv').disabled = !B.length;
  const first = B.length ? histStart(B[0]) : hist.fetchedAt;
  $('histNote').textContent = B.length
    ? `${histBuckets(B.length)}${hist.period === 3600 ? '' : ` of ${histPeriodText()}`} from ${fmtDay(first)} ${fmtClock(first)} to now (the ${hist.role} board’s record since it booted; up to four days, lost on reset). Loaded ${fmtClock(hist.fetchedAt)}.`
    : 'No history yet.';
  if (!B.length) return;
  renderHistTiles();
  const peerName = hist.role === 'house' ? 'at gate' : 'at house';
  $('histLegend').innerHTML = (hist.peer
    ? `<span><span class="sw line here"></span> here (${esc(hist.role)})</span><span><span class="sw line peer"></span> ${peerName}</span>`
    : '')
    + '<span><span class="sw band"></span> range to the worst value</span>'
    + '<span><span class="sw heat"></span> more problems = darker</span>';
  drawHistory();
  renderHistTable();
}

function tile(label, value, sub) {
  return `<div class="tile"><div class="t-label">${esc(label)}</div><div class="t-value">${esc(value)}</div><div class="t-sub">${esc(sub)}</div></div>`;
}

function renderHistTiles() {
  const B = hist.buckets, peer = hist.peer;
  const span = sum(B, histSpan), down = sum(B, (b) => b.down_s);
  const up = 100 * (1 - down / span);
  const availability = tile('Link up', down ? `${up.toFixed(up > 99.9 ? 2 : 1)} %` : '100 %',
    down ? `down ${fmtDur(down * 1000)} of ${fmtDur(span * 1000)}` : `no downtime in ${fmtDur(span * 1000)}`);

  // Worst SNR margin: the lowest per-frame SNR in any bucket, either direction.
  const floor = SNR_FLOOR[hist.sf];
  let worst = null;
  for (const b of B) {
    for (const [v, side] of [[b.snr_min, 'here'], [peer ? b.peer_snr_min : null, hist.role === 'house' ? 'at gate' : 'at house']]) {
      if (v !== null && v !== undefined && (!worst || v < worst.v)) worst = { v, side, at: histStart(b) };
    }
  }
  const margin = floor === undefined || !worst
    ? tile('Worst SNR', worst ? `${fmtNum(worst.v)} dB` : '—', worst ? `${worst.side}, ${fmtDay(worst.at)} ${fmtClock(worst.at)}` : 'no frames received')
    : tile('Worst SNR margin', `${fmtNum(worst.v - floor)} dB`,
      `above the SF${hist.sf} limit (${floor} dB), ${worst.side}, ${fmtDay(worst.at)} ${fmtClock(worst.at)}`);

  const tx = sum(B, (b) => b.tx), retries = sum(B, (b) => b.retries);
  const resends = tile('Resends', tx ? `${((100 * retries) / tx).toFixed(1)} %` : '—',
    `${retries} of ${tx} frames sent here` + (peer ? `; the gate resent ${sum(B, (b) => b.peer_retries)}` : ''));

  const lost = (b) => b.giveups + (peer ? b.peer_giveups : 0);
  const crc = sum(B, (b) => b.crc_err + (peer ? b.peer_crc_err : 0));
  const hit = B.filter((b) => lost(b) > 0).length;
  const lostTile = tile('Messages lost', String(sum(B, lost)),
    `in ${hit} of ${histBuckets(B.length)}; ${crc} CRC error${crc === 1 ? '' : 's'}`);
  $('histTiles').innerHTML = availability + margin + resends + lostTile;
}

// Clean tick values for a [lo, hi] range, about three of them.
function niceScale(lo, hi) {
  const span = Math.max(hi - lo, 1);
  const step = [1, 2, 5, 10, 20, 25, 50, 100].find((s) => span / s <= 3) || 200;
  const a = Math.floor(lo / step) * step, b = Math.ceil(hi / step) * step;
  const ticks = [];
  for (let v = a; v <= b + 1e-9; v += step) ticks.push(v);
  return { lo: a, hi: b === a ? a + step : b, ticks };
}

const HIST_PANELS = [
  { title: 'Received signal (dBm)', avg: 'rssi_avg', band: 'rssi_min' },
  { title: 'SNR margin (dB)', avg: 'snr_avg', band: 'snr_min', margin: true },
  { title: 'Noise floor (dBm)', avg: 'noise_avg', band: 'noise_max' },
];
const HIST_ROWS = [
  ['Resends', (b, p) => b.retries + (p ? b.peer_retries : 0)],
  ['Give-ups', (b, p) => b.giveups + (p ? b.peer_giveups : 0)],
  ['CRC errors', (b, p) => b.crc_err + (p ? b.peer_crc_err : 0)],
  ['Down (min)', (b) => b.down_s / 60],
];

function drawHistory() {
  const svg = $('histChart');
  const B = hist.buckets, n = B.length;
  const series = [['here', ''], ...(hist.peer ? [['peer', 'peer_']] : [])];
  const floor = SNR_FLOOR[hist.sf];
  const W = Math.max(300, Math.round(svg.parentElement.clientWidth));
  const padL = 70, padR = 10, plotW = W - padL - padR;
  // Capped bucket width, newest at the right edge: a young history doesn't stretch an hour across the card.
  const bw = Math.min(plotW / n, 28), x0 = padL + plotW - n * bw;
  const cx = (i) => x0 + (i + 0.5) * bw;
  const PH = 84, TITLE = 28, GAP = 14;
  let y = 0, out = '';
  const panels = [];

  for (const p of HIST_PANELS) {
    const shift = p.margin && floor !== undefined ? floor : 0;
    const title = p.margin ? (floor === undefined ? 'SNR (dB)' : `SNR margin above the SF${hist.sf} limit (dB)`) : p.title;
    const val = (b, pre, k) => (b[pre + k] === null || b[pre + k] === undefined ? null : b[pre + k] - shift);
    const vals = B.flatMap((b) => series.flatMap(([, pre]) => [val(b, pre, p.avg), val(b, pre, p.band)])).filter((v) => v !== null);
    if (p.margin) vals.push(0);
    if (!vals.length) vals.push(0);
    const sc = niceScale(Math.min(...vals), Math.max(...vals));
    const top = y + TITLE;
    const py = (v) => top + PH - ((v - sc.lo) / (sc.hi - sc.lo)) * PH;
    out += `<text class="ptitle" x="0" y="${y + 13}">${esc(title)}</text>`;
    for (const t of sc.ticks) {
      out += `<line class="${t === 0 && p.margin ? 'zero' : 'grid'}" x1="${padL}" x2="${W - padR}" y1="${py(t)}" y2="${py(t)}"/>`
        + `<text x="${padL - 6}" y="${py(t) + 4}" text-anchor="end">${t}</text>`;
    }
    if (p.margin) out += `<text x="${W - padR}" y="${py(0) - 4}" text-anchor="end">limit</text>`;
    for (const [cls, pre] of series) {
      // Runs of buckets with data: a band from the average to the worst value, and the average as a line.
      let run = [];
      const flush = () => {
        if (!run.length) return;
        const avg = run.map((i) => `${cx(i).toFixed(1)},${py(val(B[i], pre, p.avg)).toFixed(1)}`);
        const band = run.map((i) => `${cx(i).toFixed(1)},${py(val(B[i], pre, p.band)).toFixed(1)}`).reverse();
        if (run.length > 1) {
          out += `<polygon class="band ${cls}" points="${avg.concat(band).join(' ')}"/>`
            + `<polyline class="ln ${cls}" points="${avg.join(' ')}"/>`;
        } else {
          out += `<line class="ln ${cls}" x1="${cx(run[0]) - 3}" x2="${cx(run[0]) + 3}" y1="${avg[0].split(',')[1]}" y2="${avg[0].split(',')[1]}"/>`;
        }
        run = [];
      };
      B.forEach((b, i) => (val(b, pre, p.avg) === null ? flush() : run.push(i)));
      flush();
    }
    panels.push({ val, py, series, p });
    y = top + PH + GAP;
  }

  // Problem strip: one row per counter, each shaded against its own busiest bucket (values in the tooltip/table).
  out += `<text class="ptitle" x="0" y="${y + 13}">Problems per ${hist.period === 3600 ? 'hour' : `${histPeriodText()} bucket`}</text>`;
  const RH = 14, rowTop = y + TITLE, gap = bw >= 6 ? 2 : 0;
  HIST_ROWS.forEach(([label, f], r) => {
    const ry = rowTop + r * (RH + 3);
    const vals = B.map((b) => f(b, hist.peer));
    const max = Math.max(...vals);
    out += `<text x="${padL - 6}" y="${ry + RH - 3}" text-anchor="end">${esc(label)}</text>`
      + `<rect class="track" x="${padL}" y="${ry}" width="${plotW}" height="${RH}" rx="2"/>`;
    vals.forEach((v, i) => {
      if (!v) return;
      const pct = Math.round(30 + (70 * v) / max);
      out += `<rect x="${(x0 + i * bw + gap / 2).toFixed(1)}" y="${ry}" width="${Math.max(1, bw - gap).toFixed(1)}" height="${RH}" rx="2"`
        + ` style="fill: color-mix(in oklab, var(--chart-heat) ${pct}%, var(--surface))"/>`;
    });
  });
  y = rowTop + HIST_ROWS.length * (RH + 3) + 4;

  // Time axis: bucket start times, spaced so labels don't touch, counted back from the newest.
  const k = [1, 2, 3, 4, 6, 8, 12, 24, 48, 96].find((s) => s * bw >= 100) || n; // room for "Mon 08:43 PM"
  let lastDay = '';
  for (let i = 0; i < n; i++) {
    if ((n - 1 - i) % k) continue;
    const t = histStart(B[i]), day = fmtDay(t);
    const label = day !== lastDay ? `${day} ${fmtClock(t)}` : fmtClock(t);
    lastDay = day;
    const x = Math.min(Math.max(cx(i), x0 + 40), W - padR - 40);
    out += `<text x="${x}" y="${y + 12}" text-anchor="middle">${esc(label)}</text>`;
  }
  const H = y + 18;

  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.setAttribute('height', H);
  svg.innerHTML = `${out}<g id="histHover"></g><rect class="hit" x="${x0}" y="0" width="${n * bw}" height="${H}"/>`;
  histLayout = { W, x0, bw, cx, H, panels, chartBottom: y };
  drawHistHover();
}

function drawHistHover() {
  const g = $('histHover'), tip = $('histTip');
  if (!g || !histLayout) return;
  if (histAt < 0 || histAt >= hist.buckets.length) {
    g.innerHTML = '';
    tip.hidden = true;
    return;
  }
  const { cx, panels, chartBottom } = histLayout;
  const b = hist.buckets[histAt], x = cx(histAt);
  let s = `<line class="cross" x1="${x}" x2="${x}" y1="18" y2="${chartBottom}"/>`;
  for (const { val, py, series, p } of panels) {
    for (const [cls, pre] of series) {
      const v = val(b, pre, p.avg);
      if (v !== null) s += `<circle class="dot ${cls}" cx="${x}" cy="${py(v)}" r="4"/>`;
    }
  }
  g.innerHTML = s;
  renderHistTip(b);
  // Beside the crosshair, on whichever side has room.
  const w = tip.offsetWidth, plot = $('histPlot').clientWidth;
  const px = (x / histLayout.W) * plot;
  tip.style.left = `${px + 14 + w > plot ? Math.max(0, px - 14 - w) : px + 14}px`;
}

// Built with textContent: values lead, the series name follows.
function renderHistTip(b) {
  const tip = $('histTip');
  tip.replaceChildren();
  const add = (cls, text, parent = tip) => {
    const el = document.createElement('div');
    el.className = cls;
    el.textContent = text;
    parent.appendChild(el);
    return el;
  };
  const row = (key, value, label) => {
    const r = add('r', '');
    const k = document.createElement('span');
    k.className = `key ${key}`;
    const v = document.createElement('b');
    v.textContent = value;
    const l = document.createElement('span');
    l.textContent = label;
    r.append(k, v, l);
  };
  const t = histStart(b), end = t + histSpan(b) * 1000;
  add('tt', `${fmtDay(t)} ${fmtClock(t)}–${fmtClock(end)}${b.idx === hist.current ? ' (so far)' : ''}`);
  const floor = SNR_FLOOR[hist.sf];
  const peerName = hist.role === 'house' ? 'at gate' : 'at house';
  const sides = [['here', '', 'here'], ...(hist.peer ? [['peer', 'peer_', peerName]] : [])];
  add('sec', 'Signal · SNR');
  for (const [key, pre, name] of sides) {
    const m = b[`${pre}snr_min`];
    row(key, `${fmtNum(b[`${pre}rssi_avg`])} dBm`, `${name}, worst ${fmtNum(b[`${pre}rssi_min`])}`);
    row(key, `${fmtNum(b[`${pre}snr_avg`])} dB`, `SNR ${name}, worst ${fmtNum(m)}${floor !== undefined && m !== null ? ` (margin ${fmtNum(m - floor)})` : ''}`);
  }
  add('sec', 'Noise floor');
  for (const [key, pre, name] of sides) row(key, `${fmtNum(b[`${pre}noise_avg`])} dBm`, `${name}, peak ${fmtNum(b[`${pre}noise_max`])}`);
  add('sec', 'Traffic');
  const both = (here, peer) => (hist.peer ? `${here} here · ${peer} gate` : String(here));
  row('', `${b.tx} / ${b.rx}`, 'frames sent / received');
  row('', both(b.retries, b.peer_retries), 'resends');
  row('', both(b.giveups, b.peer_giveups), 'give-ups');
  row('', both(b.crc_err, b.peer_crc_err), 'CRC errors');
  row('', b.down_s ? fmtDur(b.down_s * 1000) : '0', 'link down');
  tip.hidden = false;
}

function histPointer(e) {
  if (!histLayout) return;
  const r = $('histChart').getBoundingClientRect();
  const x = ((e.clientX - r.left) / r.width) * histLayout.W;
  const i = Math.floor((x - histLayout.x0) / histLayout.bw);
  const next = i >= 0 && i < hist.buckets.length ? i : -1;
  if (next !== histAt) {
    histAt = next;
    drawHistHover();
  }
}

function histKey(e) {
  if (!hist?.buckets.length) return;
  const n = hist.buckets.length;
  const at = histAt < 0 ? n - 1 : histAt;
  const next = { ArrowLeft: at - 1, ArrowRight: at + 1, Home: 0, End: n - 1 }[e.key];
  if (e.key === 'Escape') histAt = -1;
  else if (next === undefined) return;
  else histAt = Math.min(n - 1, Math.max(0, histAt < 0 ? n - 1 : next));
  e.preventDefault();
  drawHistHover();
}

function renderHistTable() {
  const peer = hist.peer, peerName = hist.role === 'house' ? 'gate' : 'house';
  const cols = [
    ['Start', (b) => `${fmtDay(histStart(b))} ${fmtClock(histStart(b))}`],
    ['RSSI avg / worst (dBm)', (b) => `${fmtNum(b.rssi_avg)} / ${fmtNum(b.rssi_min)}`],
    ['SNR avg / worst (dB)', (b) => `${fmtNum(b.snr_avg)} / ${fmtNum(b.snr_min)}`],
    ['Noise avg / peak (dBm)', (b) => `${fmtNum(b.noise_avg)} / ${fmtNum(b.noise_max)}`],
    ...(peer ? [
      [`RSSI at ${peerName}`, (b) => `${fmtNum(b.peer_rssi_avg)} / ${fmtNum(b.peer_rssi_min)}`],
      [`SNR at ${peerName}`, (b) => `${fmtNum(b.peer_snr_avg)} / ${fmtNum(b.peer_snr_min)}`],
      [`Noise at ${peerName}`, (b) => `${fmtNum(b.peer_noise_avg)} / ${fmtNum(b.peer_noise_max)}`],
    ] : []),
    ['TX / RX', (b) => `${b.tx} / ${b.rx}`],
    ['Resends', (b) => (peer ? `${b.retries} + ${b.peer_retries}` : b.retries)],
    ['Give-ups', (b) => (peer ? `${b.giveups} + ${b.peer_giveups}` : b.giveups)],
    ['CRC errors', (b) => (peer ? `${b.crc_err} + ${b.peer_crc_err}` : b.crc_err)],
    ['Down', (b) => (b.down_s ? fmtDur(b.down_s * 1000) : '0')],
  ];
  const rows = [...hist.buckets].reverse();
  $('histTable').innerHTML = `<thead><tr>${cols.map(([h]) => `<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>`
    + rows.map((b) => `<tr>${cols.map(([, f]) => `<td>${esc(f(b))}</td>`).join('')}</tr>`).join('')
    + `</tbody>${peer ? `<caption class="muted small" style="caption-side: bottom; text-align: left">Pairs “a + b”: here + ${peerName}.</caption>` : ''}`;
}

function historyCsv() {
  const fields = Object.keys(hist.buckets[0]);
  const pad = (v) => String(v).padStart(2, '0');
  const stamp = (ms) => {
    const d = new Date(ms);
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  };
  return [['start', ...fields].join(','),
    ...hist.buckets.map((b) => [stamp(histStart(b)), ...fields.map((f) => (b[f] === null ? '' : b[f]))].join(','))].join('\n') + '\n';
}

function onEvent(ev) {
  switch (ev.event) {
    case 'log':
      logLine(`[${fmtDur(ev.t)}] ${ev.ev} a=${ev.a} b=${ev.b}`);
      break;
    case 'status':
      renderStatus(ev.status);
      break;
    case 'pong':
      clearTimeout(pingWait);
      pingWait = null;
      $('pingRtt').textContent = `${ev.rtt_ms} ms`;
      $('pingHere').textContent = `${ev.rssi} dBm / ${Number(ev.snr).toFixed(1)} dB`;
      $('pingPeer').textContent = `${ev.peer_rssi} dBm / ${ev.peer_snr} dB`;
      $('pingFei').textContent = fmtFei(ev.fei);
      rssiHist.push({ here: ev.rssi, peer: ev.peer_rssi });
      if (rssiHist.length > 60) rssiHist.shift();
      drawRssi();
      break;
    case 'remote_diag': {
      clearTimeout(diagTimer);
      const c = ev.counters;
      $('diagOut').textContent =
        `fw ${ev.fw} · up ${fmtDur(ev.uptime_s * 1000)}\n` +
        `tx ${c.tx} rx ${c.rx} retries ${c.retries} giveups ${c.giveups} mac_fail ${c.mac_fail} replay ${c.replay}\n` +
        Object.entries(ev.params).map(([k, v]) => `${k}=${v}`).join('  ');
      break;
    }
    case 'remote_set':
      $('remResult').textContent = ev.ok ? 'Gate accepted and saved the value.'
        : ev.applied ? 'Gate applied the value but couldn’t save it; it reverts when the gate reboots.'
        : ev.acked ? 'Gate rejected the value.' : 'No reply from gate.';
      break;
  }
}

async function relayTest(k) {
  const ms = Number($('relayMs').value);
  if (!Number.isInteger(ms) || ms < 50 || ms > 5000) return toast('Pulse length must be 50–5000 ms.', 'err');
  if (role === 'gate' && !confirm(`This will pulse the opener ${k === 1 ? 'OPEN' : 'CLOSE'} input and move the gate. Continue?`)) return;
  await call('relay.test', { k, ms });
  toast(`K${k} pulsed for ${ms} ms.`);
}

// ---------- Firmware update ----------
// The board's Arduino bootloader (SAM-BA with the Arduino X/Y/Z extensions, what bossac talks to) is
// driven over Web Serial. A 1200-baud open/close makes the running firmware reset into it; it then
// enumerates as a different USB device (PID 0x0059), which needs its own one-time port grant. Config and
// key live in the SPI flash chip (0.5.0 on), which the bootloader never touches. A failed or interrupted
// update leaves the application erased, so the bootloader stays in charge and flashing again finishes it.
const USB_VID = 0x2341;
const BOOT_PID = 0x0059;
const APP_START = 0x2000; // after the 8 KB bootloader
const APP_MAX = 0x40000 - APP_START;
const RAM_BUF = 0x20005000; // bootloader's free RAM, staged here before each flash write
const CHUNK = 4096;
const FW_MARKER = 'GATELINK_FW='; // config.h FW_MARKER_PREFIX; firmware 0.5.1 on

let flashing = false;
let latestFw = null; // firmware/latest.json, written by the Pages deploy from the latest release
let bootPick = null; // { resolve, reject } while waiting for the user to grant the bootloader port
let flashCheck = null; // what the board should report once it's back on new firmware

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hex8 = (n) => n.toString(16).toUpperCase().padStart(8, '0');
const isBootPort = (p) => {
  const i = p.getInfo();
  return i.usbVendorId === USB_VID && i.usbProductId === BOOT_PID;
};

function cmpVer(a, b) {
  const pa = String(a).split('.').map(Number);
  const pb = String(b).split('.').map(Number);
  for (let i = 0; i < 3; i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) - (pb[i] || 0);
  return 0;
}

// CRC-16/XMODEM, as the bootloader's Z command computes it.
function crc16(bytes) {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

// Checks a .bin is an application image for this board and finds its GateLink version marker.
// Returns { bytes (padded to whole 64-byte flash pages), version or null }.
function parseFirmware(buf, name) {
  const raw = new Uint8Array(buf);
  if (raw.length < 1024 || raw.length > APP_MAX) throw new Error(`${name}: ${raw.length} bytes is not a firmware image for this board.`);
  const dv = new DataView(buf);
  const sp = dv.getUint32(0, true);
  const reset = dv.getUint32(4, true);
  if (sp <= 0x20000000 || sp > 0x20008000 || !(reset & 1) || reset < APP_START || reset >= 0x40000) {
    throw new Error(`${name} is not a firmware image for this board (a .bin built for the MKR WAN 1310 is needed, not .hex or .elf).`);
  }
  let version = null;
  const text = new TextDecoder('latin1').decode(raw);
  const at = text.indexOf(FW_MARKER);
  if (at >= 0) version = /^[0-9]+\.[0-9]+\.[0-9]+/.exec(text.slice(at + FW_MARKER.length, at + FW_MARKER.length + 16))?.[0] || null;
  const bytes = new Uint8Array(Math.ceil(raw.length / 64) * 64).fill(0xff);
  bytes.set(raw);
  return { bytes, version, name };
}

class SamBa {
  constructor(p) {
    this.port = p;
    this.buf = new Uint8Array(0);
    this.wake = null;
    this.ended = false;
  }

  async open() {
    await this.port.open({ baudRate: 115200 }); // USB CDC: the rate is ignored
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this.loop = (async () => {
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          const b = new Uint8Array(this.buf.length + value.length);
          b.set(this.buf);
          b.set(value, this.buf.length);
          this.buf = b;
          this.wake?.();
        }
      } catch {}
      this.ended = true;
      this.wake?.();
    })();
  }

  async close() {
    try { await this.reader.cancel(); } catch {}
    await this.loop;
    try { this.reader.releaseLock(); } catch {}
    try { this.writer.releaseLock(); } catch {}
    await this.port.close().catch(() => {});
  }

  async write(data) {
    await this.writer.write(typeof data === 'string' ? new TextEncoder().encode(data) : data);
  }

  // Replies end in "\n\r".
  async reply(what, ms) {
    const until = Date.now() + ms;
    for (;;) {
      for (let i = 1; i < this.buf.length; i++) {
        if (this.buf[i - 1] === 10 && this.buf[i] === 13) {
          const line = new TextDecoder('latin1').decode(this.buf.subarray(0, i - 1));
          this.buf = this.buf.slice(i + 1);
          return line;
        }
      }
      if (this.ended) throw new Error(`${what}: the board went away`);
      const left = until - Date.now();
      if (left <= 0) {
        const got = this.buf.length ? ` (got ${JSON.stringify(new TextDecoder('latin1').decode(this.buf.subarray(0, 32)))})` : '';
        throw new Error(`${what}: no answer from the bootloader${got}.`);
      }
      await new Promise((r) => { this.wake = r; setTimeout(r, left); });
      this.wake = null;
    }
  }

  async cmd(text, what, ms = 2000) {
    await this.write(text);
    return this.reply(what, ms);
  }

  // Binary mode, then the version line, which must advertise the Arduino extensions used here.
  async hello() {
    await this.write('N#');
    await this.reply('N', 500).catch(() => {}); // only answered if the mode was already binary
    this.buf = new Uint8Array(0);
    const v = (await this.cmd('V#', 'version')).trim();
    const ext = /\[Arduino:([A-Z]+)\]/.exec(v)?.[1] || '';
    if (!['X', 'Y', 'Z'].every((c) => ext.includes(c))) throw new Error(`Unsupported bootloader: ${v}`);
    return v;
  }

  async erase(addr) {
    const r = await this.cmd(`X${hex8(addr)}#`, 'erase', 20000);
    if (r !== 'X') throw new Error(`erase: unexpected reply "${r}"`);
  }

  // Stage bytes in RAM, then copy them to flash. The data must reach the board as separate USB transfers
  // from its S command and from the next command (the bootloader mishandles them sharing packets; bossac
  // flushes in between). Web Serial's write() resolves once the bytes are queued, not sent, so back-to-back
  // writes can merge: wait for each to drain.
  async program(dst, data) {
    const at = `write at 0x${hex8(dst)}`;
    await this.write(`S${hex8(RAM_BUF)},${hex8(data.length)}#`);
    await sleep(20);
    await this.write(data.slice()); // own buffer: the caller's view is reused
    await sleep(30);
    let r = await this.cmd(`Y${hex8(RAM_BUF)},0#`, at);
    if (r === 'Y') r = await this.cmd(`Y${hex8(dst)},${hex8(data.length)}#`, at, 5000);
    if (r !== 'Y') throw new Error(`${at}: unexpected reply "${r}"`);
  }

  async crc(addr, len) {
    const r = await this.cmd(`Z${hex8(addr)},${hex8(len)}#`, 'verify', 10000);
    const m = /^Z([0-9A-Fa-f]{8})#$/.exec(r);
    if (!m) throw new Error(`verify: unexpected reply "${r}"`);
    return parseInt(m[1], 16);
  }

  // SYSRESETREQ: the bootloader sees a valid application and starts it. No reply.
  async reset() {
    await this.write('WE000ED0C,05FA0004#');
  }
}

function fwStep(text, cls = '') {
  $('fwStep').textContent = text;
  $('fwStep').className = `small ${cls || 'muted'}`;
  if (text) logLine(`firmware: ${text}`, cls === 'bad' ? 'err' : '');
}

function setFlashing(on) {
  flashing = on;
  $('btnFwLatest').disabled = on;
  $('fwFile').disabled = on;
  $('btnConnect').disabled = on || !('serial' in navigator);
  $('fwProgress').hidden = !on;
  if (!on) $('fwBootRow').hidden = true;
}

function updateFwCard() {
  $('fwBoard').textContent = boardInfo ? `${boardInfo.fw} (${boardInfo.role})` : '—';
  if (!latestFw) return;
  const rel = boardInfo ? cmpVer(latestFw.version, boardInfo.fw) : 1;
  const newer = boardInfo && rel > 0;
  $('fwLatest').textContent = `${latestFw.version}${newer ? ' · update available' : ''}`;
  $('fwLatest').className = newer ? 'good' : '';
  // Only an update is the primary action; reinstalling the same version or going back is offered plainly.
  const b = $('btnFwLatest');
  b.textContent = rel > 0 ? `Install ${latestFw.version}` : rel === 0 ? `Reinstall ${latestFw.version}` : `Install ${latestFw.version} (older)`;
  b.classList.toggle('primary', rel > 0);
}

async function loadLatest() {
  try {
    const r = await fetch('firmware/latest.json', { cache: 'no-cache' });
    if (!r.ok) return;
    latestFw = await r.json();
    $('btnFwLatest').hidden = false;
    updateFwCard();
  } catch {} // served without the bundle (local copy): the file picker still works
}

async function installLatest() {
  const r = await fetch(`firmware/${latestFw.file}`, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`Couldn't download ${latestFw.file} (${r.status}).`);
  const buf = await r.arrayBuffer();
  const sum = [...new Uint8Array(await crypto.subtle.digest('SHA-256', buf))].map((x) => x.toString(16).padStart(2, '0')).join('');
  if (sum !== latestFw.sha256) throw new Error(`${latestFw.file} is damaged (checksum mismatch). Reload the page and try again.`);
  await flashFirmware(parseFirmware(buf, latestFw.file));
}

async function flashFile(file) {
  const fw = parseFirmware(await file.arrayBuffer(), file.name);
  if (!fw.version && !confirm(`${file.name} has no GateLink version marker (firmware before 0.5.1 has none, other sketches neither). Flash it anyway?`)) return;
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

async function pickBootPort() {
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
  if (flashing) return;
  const label = fw.version ? `firmware ${fw.version}` : fw.name;
  let target = port;
  if (port) {
    const info = boardInfo || {};
    let msg = `Flash ${label} to this ${info.role || ''} board (running ${info.fw || '?'})?`;
    if (fw.version && info.fw && cmpVer(fw.version, info.fw) < 0) msg += '\n\nThat is older than the firmware it runs now.';
    msg += '\n\nRelays release and the link is down for about a minute.';
    if (info.cfg_store !== 'spi') msg += '\n\nThis board keeps config and key in program flash, which the update erases: export the config (Config tab) and have the key ready.';
    else if (appliedUnsaved) msg += '\n\nApplied settings that haven’t been saved to flash will be lost.';
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
      if (port) await disconnect(true);
      lastPort = target;
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
        if (i >= 10) throw new Error(`Couldn't open the bootloader port: ${e.message}`);
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
    flashCheck = { version: fw.version, label };
    startReconnect();
  } catch (e) {
    fwStep(`${e.message} If the update had started, the board waits in its bootloader (LED fading): flash again to finish it.`, 'bad');
    throw new Error(`Firmware update failed: ${e.message}`);
  } finally {
    await sb?.close();
    setFlashing(false);
  }
}

// After an update the board comes back on its new firmware: show what it reports.
function reportFlash(info) {
  const c = flashCheck;
  flashCheck = null;
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

// ---------- Log ----------
const logLines = []; // what Download saves; the view keeps only the last 1000
const LOG_KEEP = 20000;
function logLine(text, cls = '') {
  const stamp = new Date().toLocaleTimeString();
  logLines.push(`${stamp} ${text}`);
  if (logLines.length > LOG_KEEP) logLines.splice(0, logLines.length - LOG_KEEP);
  const div = document.createElement('div');
  div.textContent = `${stamp} ${text}`;
  if (cls) div.className = cls;
  const view = $('logView');
  const atBottom = view.scrollTop + view.clientHeight >= view.scrollHeight - 20;
  view.appendChild(div);
  while (view.childElementCount > 1000) view.firstChild.remove();
  if (atBottom) view.scrollTop = view.scrollHeight;
}

// Non-modal notice in a role="status" region (always in the DOM so screen readers announce it; empty =
// invisible). Errors stay up longer. Click to dismiss.
let toastTimer = null;
function hideToast() {
  clearTimeout(toastTimer);
  $('toast').textContent = '';
}
function toast(msg, kind = '') {
  logLine(msg, kind === 'err' ? 'err' : '');
  const t = $('toast');
  t.textContent = msg;
  t.className = `toast ${kind}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(hideToast, kind === 'err' ? 12000 : 5000);
}

// ---------- Wiring ----------
// Runs a click handler, reporting errors as a toast. The clicked button is disabled until the handler
// finishes so a slow board reply can't be double-submitted.
function guard(fn) {
  return async (...a) => {
    const b = a[0]?.currentTarget instanceof HTMLButtonElement ? a[0].currentTarget : null;
    if (b) b.disabled = true;
    try { await fn(...a); } catch (e) { toast(e.message, 'err'); } finally {
      if (b) b.disabled = !port && !OFFLINE_OK.includes(b.id);
    }
  };
}

// Key input: accept pasted keys with spaces, colons or dashes; flag anything that isn't 32 hex chars.
function keyValue() {
  return $('keyInput').value.replace(/[\s:-]/g, '').toLowerCase();
}
function checkKeyInput() {
  const k = keyValue();
  const ok = /^[0-9a-f]{32}$/.test(k);
  $('keyInput').setAttribute('aria-invalid', String(!!k && !ok));
  $('keyHint').textContent = !k || ok ? '' : `${k.length}/32 characters${/[^0-9a-f]/.test(k) ? ', hex digits only (0–9, a–f)' : ''}`;
}

function init() {
  if (!('serial' in navigator)) {
    $('unsupported').hidden = false;
    $('btnConnect').disabled = true;
  }
  setConnected(false);
  renderWiring(wiringShown);
  applyRole('unset');
  document.querySelectorAll('[data-wiring]').forEach((b) => b.addEventListener('click', () => renderWiring(b.dataset.wiring)));
  new ResizeObserver(([e]) => {
    const w = e.contentRect.width;
    if (!w || (w < 600) === wiringNarrow) return; // 0 = Install tab hidden
    wiringNarrow = w < 600;
    renderWiring(wiringShown);
  }).observe(document.querySelector('.wiring'));

  // Tabs: click or arrow keys (roving tabindex, per the ARIA tabs pattern).
  const tabs = [...document.querySelectorAll('#tabs [role=tab]')];
  const selectTab = (b) => {
    tabs.forEach((x) => {
      const on = x === b;
      x.classList.toggle('active', on);
      x.setAttribute('aria-selected', String(on));
      x.tabIndex = on ? 0 : -1;
    });
    document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.id === `tab-${b.dataset.tab}`));
    history.replaceState(null, '', `#${b.dataset.tab}`); // survives a page reload
  };
  tabs.forEach((b, i) => {
    b.addEventListener('click', () => selectTab(b));
    b.addEventListener('keydown', (e) => {
      const j = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: tabs.length - 1 }[e.key];
      if (j === undefined) return;
      e.preventDefault();
      const next = tabs[(j + tabs.length) % tabs.length];
      selectTab(next);
      next.focus();
    });
  });
  const tabFromHash = () => {
    const b = tabs.find((x) => `#${x.dataset.tab}` === location.hash);
    if (b) selectTab(b);
  };
  tabFromHash();
  window.addEventListener('hashchange', tabFromHash); // back/forward, or a #tab link
  $('toast').onclick = hideToast;
  // Chrome steps a focused number field on mouse wheel: scrolling the page past one (freq_hz, say) would
  // silently change it. Drop focus instead, so the wheel scrolls the page.
  document.addEventListener('wheel', (e) => {
    if (e.target instanceof HTMLInputElement && e.target.type === 'number' && e.target === document.activeElement) e.target.blur();
  }, { passive: true });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && $('toast').textContent) hideToast(); });
  // Unapplied form edits are lost on reload/close.
  window.addEventListener('beforeunload', (e) => { if (flashing || (meta.length && dirtyCount())) e.preventDefault(); });

  $('btnConnect').onclick = connect;
  $('btnPickAdd').onclick = addBoard;
  $('btnPickRefresh').onclick = pickRefresh;
  $('btnPickCancel').onclick = closePicker;
  $('boardPicker').addEventListener('close', () => pickGen++); // Escape
  $('boardPicker').addEventListener('click', (e) => { // backdrop clicks land on the dialog too, outside its box
    const r = $('boardPicker').getBoundingClientRect();
    if (e.target === $('boardPicker') && (e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom)) closePicker();
  });
  $('btnDisconnect').onclick = () => { stopReconnect(); disconnect(); };
  $('btnIdentify').onclick = guard(async () => { await call('identify'); toast('LED strobing for 6 s.'); });

  $('btnCfgLoad').onclick = guard(async () => {
    const n = dirtyCount();
    if (n && !confirm(`Discard ${n} unapplied edit${n === 1 ? '' : 's'} and reload from the board?`)) return;
    await loadConfig();
  });
  $('btnCfgApply').onclick = guard(applyConfig);
  // Setting tooltips show on hover or focus; a tap or click pins one open (touch has no hover).
  const closeTips = (except) => document.querySelectorAll('.tip.show').forEach((t) => t !== except && t.classList.remove('show'));
  document.addEventListener('click', (e) => {
    const info = e.target.closest('.info');
    const tip = info?.nextElementSibling;
    closeTips(tip);
    tip?.classList.toggle('show');
  });
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape') return;
    closeTips();
    if (document.activeElement?.classList.contains('info')) document.activeElement.blur();
  });
  $('btnCfgSave').onclick = guard(async () => {
    // Save writes what the board is running; edits still in the form would silently be left out.
    const n = dirtyCount();
    if (n) return toast(`${n} edit${n === 1 ? ' isn’t' : 's aren’t'} applied yet. Apply first, then Save.`, 'err');
    await call('config.save');
    markUnsaved(false);
    toast('Saved to flash.');
  });
  $('btnReboot').onclick = guard(async () => {
    const lose = appliedUnsaved ? '\n\nApplied settings haven’t been saved to flash and will be lost.' : '';
    if (!confirm(`Reboot the board? Relays release during reboot.${lose}`)) return;
    await request('reboot', {}, 1500).catch(() => {});
    await disconnect(true);
    startReconnect();
    toast('Board rebooting — reconnecting…');
  });
  $('btnCfgReset').onclick = guard(async () => {
    if (!confirm('Erase config and key and restore defaults?')) return;
    await call('config.reset');
    markUnsaved(false); // reset saves the defaults
    await loadConfig();
    await refreshInfo();
    toast('Defaults restored. Reboot to apply role.');
  });
  $('btnCfgExport').onclick = () =>
    download(`gatelink-${role}-config-${new Date().toISOString().slice(0, 10)}.json`, JSON.stringify({ role, params: formValues(false) }, null, 2));
  $('fileImport').onchange = (e) => { if (e.target.files[0]) importConfig(e.target.files[0]); e.target.value = ''; };

  $('btnKeyGen').onclick = () => {
    const b = crypto.getRandomValues(new Uint8Array(16));
    $('keyInput').value = [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
    checkKeyInput();
  };
  $('keyInput').addEventListener('input', checkKeyInput);
  $('keyInput').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !$('btnKeySet').disabled) $('btnKeySet').click(); });
  $('btnKeyCopy').onclick = async () => {
    const key = keyValue();
    if (!key) return toast('Nothing to copy: generate or enter a key first.', 'err');
    try {
      await navigator.clipboard.writeText(key);
      toast('Key copied to the clipboard.');
    } catch (e) {
      toast(`Copy failed: ${e.message}`, 'err');
    }
  };
  $('btnKeySet').onclick = guard(async () => {
    const key = keyValue();
    if (!/^[0-9a-f]{32}$/.test(key)) return toast('Key must be exactly 32 hex characters.', 'err');
    if (keySet && !confirm('This board already has a key. Replacing it takes the link down until the other board gets the same key. Continue?')) return;
    await call('key.set', { key });
    await refreshInfo();
    toast('Key written and saved. Write the same key to the other board.');
  });

  $('btnK1').onclick = guard(() => relayTest(1));
  $('btnK2').onclick = guard(() => relayTest(2));
  $('btnPing').onclick = guard(ping);
  $('pingAuto').onchange = (e) => {
    clearInterval(pingTimer);
    if (e.target.checked) pingTimer = setInterval(() => ping().catch(() => {}), 3000);
  };
  $('btnDiag').onclick = guard(async () => {
    clearTimeout(diagTimer);
    $('diagOut').textContent = 'waiting for gate…';
    // Armed before the request: the remote_diag event clears it, however soon it arrives.
    diagTimer = setTimeout(() => {
      $('diagOut').textContent = `No reply from gate within ${DIAG_TIMEOUT_MS / 1000} s. Check the link and try again.`;
    }, DIAG_TIMEOUT_MS);
    try {
      await call('remote.diag');
    } catch (e) {
      clearTimeout(diagTimer);
      $('diagOut').textContent = `Failed: ${e.message}`;
      throw e;
    }
  });
  $('remParam').onchange = syncRemValue;
  $('remValue').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !$('btnRemSet').disabled) $('btnRemSet').click(); });
  $('btnRemSet').onclick = guard(async () => {
    const name = $('remParam').value;
    const raw = $('remValue').value.trim();
    const value = Number(raw);
    if (raw === '' || !Number.isInteger(value)) {
      $('remResult').textContent = 'Enter an integer value.';
      return;
    }
    $('remResult').textContent = 'sending…';
    try {
      await call('remote.set', { name, value });
    } catch (e) {
      // e.g. "busy" while the previous remote set is still pending, or an out-of-range value.
      $('remResult').textContent = e.message === 'busy' ? 'Busy: the previous remote set is still pending; wait for its result.' : `Not sent: ${e.message}`;
      throw e;
    }
  });
  $('btnReplay').onclick = guard(async () => {
    await call('debug.replay');
    toast('Replay sent. Check the other board’s replay counter (Status → Link).');
  });

  $('btnHistLoad').onclick = guard(async () => {
    await loadHistory();
    $('btnHistLoad').textContent = 'Refresh';
  });
  $('btnHistCsv').onclick = () => hist && download(`gatelink-${hist.role}-link-history.csv`, historyCsv(), 'text/csv');
  $('btnHistClear').onclick = guard(async () => {
    if (!confirm('Empty this board’s link history and start recording again from now?')) return;
    await call('hist.clear');
    await loadHistory();
    toast('Link history cleared.');
  });
  const chart = $('histChart');
  chart.addEventListener('pointermove', histPointer);
  chart.addEventListener('pointerdown', histPointer); // touch: tap a bucket
  chart.addEventListener('pointerleave', () => { histAt = -1; drawHistHover(); });
  chart.addEventListener('keydown', histKey);
  chart.addEventListener('blur', () => { histAt = -1; drawHistHover(); });
  // Redraw at the new width (text stays at its real size); also when the Tools tab is first shown.
  let histWidth = 0;
  new ResizeObserver(() => {
    const w = $('histPlot').clientWidth; // a redraw changes the height only, which mustn't trigger another
    if (w && w !== histWidth && hist?.buckets.length) drawHistory();
    histWidth = w;
  }).observe($('histPlot'));

  loadLatest();
  $('btnFwLatest').onclick = guard(installLatest);
  $('fwFile').onchange = guard(async (e) => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) await flashFile(f);
  });
  $('btnBootPort').onclick = pickBootPort;

  $('btnLogGet').onclick = guard(async () => {
    const res = await call('log.get');
    logLine(`--- board log (${res.log.length} entries, board uptime ${fmtDur(res.now)}) ---`);
    for (const e of res.log) logLine(`[${fmtDur(e.t)}] ${e.ev} a=${e.a} b=${e.b}`);
  });
  $('btnLogClear').onclick = () => { $('logView').innerHTML = ''; logLines.length = 0; };
  $('btnLogSave').onclick = () => {
    const stamp = new Date().toISOString().slice(0, 19).replace(/[T:]/g, '-');
    download(`gatelink-${role}-log-${stamp}.txt`, logLines.join('\n') + '\n', 'text/plain');
  };

  navigator.serial?.addEventListener('disconnect', (e) => {
    if ($('boardPicker').open) pickRefreshSoon();
    if (e.target === port) connectionLost();
  });
  // A granted port reappearing while we're waiting for a reboot is the board coming back (the other
  // board never left, so it can't fire this).
  navigator.serial?.addEventListener('connect', (e) => {
    if ($('boardPicker').open) pickRefreshSoon();
    if (!reconnectUntil || port || isBootPort(e.target)) return;
    lastPort = e.target;
    tryReconnect(lastPort);
  });
}

init();
