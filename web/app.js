// GateLink web console: talks newline-delimited JSON to the board over Web Serial.
'use strict';

const $ = (id) => document.getElementById(id);

// ---------- Parameter presentation ----------
const GROUPS = [
  ['General', ['role', 'net_id']],
  ['Radio (must match on both boards)', ['freq_hz', 'sf', 'bw_hz', 'cr', 'tx_power', 'sync_word']],
  ['Link', ['retries', 'heartbeat_s', 'link_timeout_s', 'cmd_ttl_s']],
  ['Inputs', ['debounce_ms', 'in1_invert', 'in2_invert', 'in3_invert', 'in4_invert', 'power_sense']],
  ['Gate node', ['pulse_ms', 'travel_timeout_s']],
  ['House node', ['ctrl_sync', 'sync_window_ms', 'resync_ms', 'mismatch_timeout_s', 'sensor_invert', 'linkloss_open']],
];
const HELP = {
  role: 'Reboot after saving',
  net_id: 'Frames with another id are ignored',
  freq_hz: 'US: 902–928 MHz, EU: 863–870 MHz',
  sf: 'Higher = longer range, slower',
  bw_hz: '500 kHz recommended for US single channel',
  cr: 'Coding rate 4/x',
  tx_power: 'dBm (2–20)',
  sync_word: 'Private network byte',
  retries: 'Resends before giving up',
  heartbeat_s: 'Gate status interval',
  link_timeout_s: 'No frames for this long = link down',
  cmd_ttl_s: 'Drop a command not delivered within this time',
  debounce_ms: 'Input debounce',
  in1_invert: 'House: controller input · Gate: open limit',
  in2_invert: 'House: spare IN2 · Gate: closed limit',
  in3_invert: 'House: spare IN3 · Gate: opener power sense',
  in4_invert: 'Spare input IN4 (A4)',
  power_sense: 'Gate: IN3 senses opener 24 V; without it the gate reads “no power” and refuses commands',
  pulse_ms: 'OPEN/CLOSE contact closure length',
  travel_timeout_s: 'Report timeout if limit not reached',
  ctrl_sync: 'Drive K1 so the controller mirrors the gate',
  sync_window_ms: 'Ignore controller edges caused by K1',
  resync_ms: 'K1 off-time when forcing a resync',
  mismatch_timeout_s: 'Controller ≠ gate this long → resync',
  sensor_invert: 'Invert contact sensor output (K2)',
  linkloss_open: 'Sensor reads open when link is down',
};
const SELECTS = {
  role: [[0, 'unset'], [1, 'house'], [2, 'gate']],
  bw_hz: [[125000, '125 kHz'], [250000, '250 kHz'], [500000, '500 kHz']],
};
const IO_LABELS = {
  house: { in1: 'IN1 · Controller input', in2: 'IN2 · spare', in3: 'IN3 · spare', in4: 'IN4 · spare', k1: 'K1 · Controller sync', k2: 'K2 · Contact sensor' },
  gate: { in1: 'IN1 · Open limit', in2: 'IN2 · Closed limit', in3: 'IN3 · Opener power', in4: 'IN4 · spare', k1: 'K1 · OPEN pulse', k2: 'K2 · CLOSE pulse' },
  unset: { in1: 'IN1', in2: 'IN2', in3: 'IN3', in4: 'IN4', k1: 'K1', k2: 'K2' },
};

// ---------- Field wiring (Install tab) ----------
// kind: out = a relay contact the board switches, in = a signal the board reads, pwr = power.
// rows: [board terminal, device terminal]. All board GND terminals are common.
// Inputs use the internal pull-down: active = driven to 3.3 V, unwired/open = off.
const V33 = '3.3 V (VCC)';
const SPARE_INPUTS = { name: 'Spare inputs (optional)', hint: 'Spare · e.g. beam break, alarm status', kind: 'in',
  rows: [['IN2 (A2)', 'Contact'], ['IN3 (A3)', 'Contact'], ['IN4 (A4)', 'Contact'], [V33, 'Common']] };
const SPARE_NOTE = 'IN2 (A2), IN3 (A3) and IN4 (A4) are spare inputs (contact to 3.3 V, internal pull-down), reserved for future use such as a beam-break sensor or alarm status. They are shown and logged but don’t affect behaviour yet. Leave spare inputs unwired if you don’t need them; they read “off”.';
const WIRING = {
  house: {
    groups: [
      { name: 'Controller relay output', hint: 'Dry contact · closed = open gate', kind: 'in',
        rows: [['IN1 (A1)', 'Contact'], [V33, 'Contact']] },
      { name: 'Controller switch input', hint: 'K1 closed while gate is not closed', kind: 'out',
        rows: [['K1 COM', 'Switch input'], ['K1 NO', 'Switch common']] },
      { name: 'Contact sensor', hint: 'K2 closed = gate closed', kind: 'out',
        rows: [['K2 COM', 'Terminal'], ['K2 NO', 'Terminal']] },
      SPARE_INPUTS,
      { name: '5 V supply', hint: 'Or USB', kind: 'pwr',
        rows: [['VIN (5 V)', '+5 V'], ['GND', '0 V']] },
    ],
    notes: [
      'IN1 reads the controller’s relay contact switched to the board’s 3.3 V (internal pull-down; open = off). The controller output must be a potential-free contact, and nothing above 3.3 V may reach IN1. Set <code>in1_invert</code> if ON and OFF come out reversed.',
      'K1 mirrors the real gate back to the controller so its switch always shows the true state. Set the controller’s switch input to toggle/follow mode (contact closed = ON, open = OFF), not detached. Wire it per the controller’s switch-input diagram. Low voltage only; never switch mains with the shield.',
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
        rows: [['IN1 (A1)', 'OUT1 · open limit'], ['IN2 (A2)', 'OUT2 · closed limit'], ['IN3 (A3)', 'OUT3 · opener 24 V'],
          ['IN4 (A4)', 'OUT4 · spare'], [V33, 'VCC (output side)'], ['GND', 'GND (output side)']] },
      { name: '24 V → 5 V buck', hint: 'Fed from opener 24 V accessory power', kind: 'pwr',
        rows: [['VIN (5 V)', '+5 V out'], ['GND', '0 V out']] },
    ],
    notes: [
      'K1/K2 go in parallel with whatever else is already on the opener’s OPEN/CLOSE inputs. GateLink only pulses them (<code>pulse_ms</code>) and never holds them, so the other devices keep working.',
      'All gate inputs go through a PNP-output opto board. Power its output side from the board’s 3.3 V, never 5 V or 24 V: a PNP output passes that voltage straight to the input pin. A lit opto drives its input to 3.3 V (active); a dead opto, missing 24 V or cut wire reads off.',
      'Opto input side (24 V): wet each limit contact from the opener’s 24 V accessory output — 24 V to AUX C, AUX NO to the opto channel input — and connect the third channel across the 24 V itself. Follow the opto board’s input markings for the common. Set AUX relay A to <em>open limit</em> and AUX relay B to <em>closed limit</em> in the opener’s menu.',
      'Gate state comes only from the limit inputs. IN3 senses opener power so a dead opener isn’t mistaken for a gate stopped between limits: with no power the gate reads “no power” and refuses commands. Set <code>power_sense</code> to 0 if IN3 isn’t wired.',
      'VIN is 5 V max. Never connect the opener’s 24 V directly to the board.',
      'Use the relays’ NO/COM contacts only. Add TVS or RC suppression on long field runs.',
      'Keep the antenna vertical and outside any metal enclosure.',
      'IN4 (A4) is spare, reserved for future use. It is shown and logged but doesn’t affect behaviour yet.',
    ],
  },
};

let wiringShown = 'house';

function renderWiring(r) {
  wiringShown = r;
  const W = WIRING[r];
  document.querySelectorAll('[data-wiring]').forEach((b) => b.classList.toggle('active', b.dataset.wiring === r));

  const esc = (t) => t.replace(/&/g, '&amp;').replace(/</g, '&lt;');
  const rowH = 28, headH = 42, gap = 14, top = 70;
  const bx = 16, bw = 250, dx = 500, dw = 250, width = 766;
  let y = top;
  let blocks = '', wires = '';
  for (const g of W.groups) {
    const h = headH + g.rows.length * rowH;
    blocks += `<rect class="blk" x="${dx}" y="${y}" width="${dw}" height="${h}" rx="8"/>`
      + `<text class="title" x="${dx + 12}" y="${y + 19}">${esc(g.name)}</text>`
      + `<text class="dim" x="${dx + 12}" y="${y + 34}">${esc(g.hint)}</text>`;
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
    + `<text class="dim" x="${bx + 12}" y="54">MKR WAN 1310 + Relay Proto Shield</text>`;
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
  let p;
  try {
    p = await navigator.serial.requestPort();
  } catch (e) {
    logLine(`connect failed: ${e.message}`, 'err');
    return;
  }
  try {
    await openPort(p);
  } catch (e) {
    logLine(`connect failed: ${e.message}`, 'err');
  }
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
    logLine(`initial query failed: ${e.message}`, 'err');
  }
  pollTimer = setInterval(() => refreshStatus().catch(() => {}), 2000);
}

function startReconnect() {
  if (!lastPort) return;
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
    logLine('auto-reconnect gave up; click Connect board', 'err');
    return;
  }
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
    writer.write(JSON.stringify({ id, cmd, ...args }) + '\n');
  });
}

async function call(cmd, args) {
  const res = await request(cmd, args);
  if (!res.ok) throw new Error(res.error || `${cmd} failed`);
  return res;
}

// ---------- UI state ----------
function setConnected(on) {
  $('btnConnect').hidden = on;
  $('btnDisconnect').hidden = !on;
  $('btnIdentify').hidden = !on;
  document.querySelectorAll('main button, main input, main select').forEach((el) => {
    if (el.closest('#tab-install')) return; // static reference, usable without a board
    if (!['logRaw', 'btnLogClear', 'btnLogSave'].includes(el.id)) el.disabled = !on;
  });
  if (!on) {
    $('devline').textContent = 'not connected';
    $('keyWarn').hidden = true;
    document.title = 'GateLink Console';
  }
}

function applyRole(r) {
  role = r;
  document.title = role === 'unset' ? 'GateLink Console' : `${role[0].toUpperCase()}${role.slice(1)} · GateLink`;
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
  $('devline').textContent = `${info.board} · fw ${info.fw} · ${info.role}`;
  $('keyWarn').hidden = info.key_set;
  $('secKeySet').textContent = info.key_set ? 'yes' : 'no (link disabled)';
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
const pill = (on) => `<span class="pill ${on ? 'on' : ''}">${on ? 'ON' : 'off'}</span>`;

async function refreshStatus() {
  const res = await call('status');
  renderStatus(res.status);
}

function renderStatus(s) {
  if (s.role !== role) applyRole(s.role);
  const gs = s.gate || 'unknown';
  const g = $('gateState');
  g.textContent = s.role === 'unset' ? 'role not set' : gs.replace('_', ' ');
  g.className = `gate-state ${gs}`;
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

  const labels = IO_LABELS[s.role] || IO_LABELS.unset;
  $('ioList').innerHTML = ['in1', 'in2', 'in3', 'in4', 'k1', 'k2'].filter((k) => k in s.io)
    .map((k) => `<div class="kv"><span>${labels[k]}</span>${pill(s.io[k])}</div>`).join('');

  $('bRole').textContent = s.reboot_pending ? `${s.role} (reboot to apply saved role)` : s.role;
  $('bFw').textContent = s.fw;
  $('bUp').textContent = fmtDur(s.uptime_ms);
  $('bReset').textContent = (s.reset_cause ?? '—').replace('_', ' ');
  const faults = s.radio_faults ? ` <span class="bad">· ${s.radio_faults} TX fault${s.radio_faults === 1 ? '' : 's'}</span>` : '';
  $('bRadio').innerHTML = (s.radio_ok ? '<span class="good">ok</span>' : '<span class="bad">not initialised</span>') + faults;
  $('keyWarn').hidden = s.key_set;

  if (s.role === 'house') {
    const r = s.remote || {};
    $('lnkRemoteRssi').textContent = r.uptime_s ? `${r.rssi} dBm / ${r.snr} dB` : '—';
    $('hCtrl').innerHTML = pill(s.ctrl);
    $('hArmed').textContent = yesNo(s.armed);
    $('hSync').textContent = `${yesNo(s.sync_window)} / ${yesNo(s.resyncing)}`;
    const cmdRes = { '-1': 'none', '-2': 'gave up', 0: 'ok', 1: 'already there', 2: 'rejected', 3: 'busy', 4: 'refused: opener unpowered' };
    $('hCmd').textContent = `#${s.cmd_id} · ${s.cmd_pending ? 'sending…' : cmdRes[s.cmd_result] ?? s.cmd_result}`;
    $('hLimits').textContent = r.uptime_s ? `open ${r.open_limit ? '●' : '○'}  closed ${r.close_limit ? '●' : '○'}` : '—';
    $('hSpare').textContent = r.uptime_s && 'in3' in r ? `power ${r.in3 ? '●' : '○'}  IN4 ${r.in4 ? '●' : '○'}` : '—';
    $('hGateUp').textContent = r.uptime_s ? fmtDur(r.uptime_s * 1000) : '—';
  }
}

// ---------- Config ----------
async function loadConfig() {
  const res = await call('config.get');
  meta = res.meta;
  params = res.params;
  renderConfig();
  const sel = $('remParam');
  sel.innerHTML = meta.filter((m) => m.remote).map((m) => `<option value="${m.name}">${m.name}</option>`).join('');
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
        input = `<input id="${id}" type="number" min="${m.min}" max="${m.max}" step="1">`;
      }
      row.innerHTML = `<label for="${id}">${name}<small>${HELP[name] || ''}</small></label>${input}`;
      card.appendChild(row);
      const el = row.querySelector('input, select');
      setField(el, params[name]);
      el.addEventListener('input', () => row.classList.toggle('dirty', fieldValue(el) !== params[name]));
    }
    form.appendChild(card);
  }
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

async function applyConfig() {
  const changes = formValues(true);
  if (!Object.keys(changes).length) return toast('No changes.');
  for (const [k, v] of Object.entries(changes)) {
    const m = meta.find((x) => x.name === k);
    if (!Number.isInteger(v) || v < m.min || v > m.max) return toast(`${k} must be an integer ${m.min}–${m.max}`);
  }
  const res = await request('config.set', { params: changes });
  await loadConfig();
  if (res.errors?.length) toast(`Rejected: ${res.errors.join(', ')}`);
  else toast(`Applied ${res.applied.join(', ')}. ${res.reboot_required ? 'Save and reboot for role change.' : 'Remember to Save.'}`);
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
  try { data = JSON.parse(await file.text()); } catch { return toast('Not a valid JSON file.'); }
  const src = data.params || data;
  for (const m of meta) {
    const el = $(`p_${m.name}`);
    if (el && Number.isInteger(src[m.name])) {
      setField(el, src[m.name]);
      el.dispatchEvent(new Event('input'));
    }
  }
  toast('Imported into the form. Review, then Apply and Save.');
}

// ---------- Tools ----------
const rssiHist = [];

function drawRssi() {
  const svg = $('rssiChart');
  if (!rssiHist.length) { svg.innerHTML = ''; return; }
  const all = rssiHist.flatMap((p) => [p.here, p.peer]);
  const lo = Math.min(...all) - 3, hi = Math.max(...all) + 3;
  const x = (i) => (rssiHist.length === 1 ? 150 : (i / (rssiHist.length - 1)) * 300);
  const y = (v) => 85 - ((v - lo) / (hi - lo || 1)) * 80;
  const line = (k, color) =>
    `<polyline stroke="var(${color})" points="${rssiHist.map((p, i) => `${x(i)},${y(p[k])}`).join(' ')}"/>`;
  svg.innerHTML = line('here', '--chart-here') + line('peer', '--chart-peer');
  $('rssiRange').textContent = `${Math.round(lo + 3)} … ${Math.round(hi - 3)} dBm`;
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
      $('pingRtt').textContent = `${ev.rtt_ms} ms`;
      $('pingHere').textContent = `${ev.rssi} dBm / ${Number(ev.snr).toFixed(1)} dB`;
      $('pingPeer').textContent = `${ev.peer_rssi} dBm / ${ev.peer_snr} dB`;
      rssiHist.push({ here: ev.rssi, peer: ev.peer_rssi });
      if (rssiHist.length > 60) rssiHist.shift();
      drawRssi();
      break;
    case 'remote_diag': {
      const c = ev.counters;
      $('diagOut').textContent =
        `fw ${ev.fw} · up ${fmtDur(ev.uptime_s * 1000)}\n` +
        `tx ${c.tx} rx ${c.rx} retries ${c.retries} giveups ${c.giveups} mac_fail ${c.mac_fail} replay ${c.replay}\n` +
        Object.entries(ev.params).map(([k, v]) => `${k}=${v}`).join('  ');
      break;
    }
    case 'remote_set':
      $('remResult').textContent = ev.ok ? 'Gate accepted and saved the value.' : ev.acked ? 'Gate rejected the value.' : 'No reply from gate.';
      break;
  }
}

async function relayTest(k) {
  const ms = Number($('relayMs').value);
  if (role === 'gate' && !confirm(`This will pulse the opener ${k === 1 ? 'OPEN' : 'CLOSE'} input and move the gate. Continue?`)) return;
  await call('relay.test', { k, ms });
}

// ---------- Log ----------
const logLines = [];
function logLine(text, cls = '') {
  const stamp = new Date().toLocaleTimeString();
  logLines.push(`${stamp} ${text}`);
  const div = document.createElement('div');
  div.textContent = `${stamp} ${text}`;
  if (cls) div.className = cls;
  const view = $('logView');
  const atBottom = view.scrollTop + view.clientHeight >= view.scrollHeight - 20;
  view.appendChild(div);
  while (view.childElementCount > 1000) view.firstChild.remove();
  if (atBottom) view.scrollTop = view.scrollHeight;
}

function toast(msg) {
  logLine(msg);
  alert(msg);
}

// ---------- Wiring ----------
function guard(fn) {
  return async (...a) => {
    try { await fn(...a); } catch (e) { toast(e.message); }
  };
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

  document.querySelectorAll('#tabs button').forEach((b) =>
    b.addEventListener('click', () => {
      document.querySelectorAll('#tabs button').forEach((x) => x.classList.toggle('active', x === b));
      document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.id === `tab-${b.dataset.tab}`));
    }));

  $('btnConnect').onclick = connect;
  $('btnDisconnect').onclick = () => { stopReconnect(); disconnect(); };
  $('btnIdentify').onclick = guard(async () => { await call('identify'); toast('LED strobing for 6 s.'); });

  $('btnCfgLoad').onclick = guard(loadConfig);
  $('btnCfgApply').onclick = guard(applyConfig);
  $('btnCfgSave').onclick = guard(async () => { await call('config.save'); toast('Saved to flash.'); });
  $('btnReboot').onclick = guard(async () => {
    if (!confirm('Reboot the board? Relays release during reboot.')) return;
    await request('reboot', {}, 1500).catch(() => {});
    await disconnect(true);
    startReconnect();
    toast('Board rebooting — reconnecting…');
  });
  $('btnCfgReset').onclick = guard(async () => {
    if (!confirm('Erase config and key and restore defaults?')) return;
    await call('config.reset');
    await loadConfig();
    await refreshInfo();
    toast('Defaults restored. Reboot to apply role.');
  });
  $('btnCfgExport').onclick = () =>
    download(`gatelink-${role}-config.json`, JSON.stringify({ role, params: formValues(false) }, null, 2));
  $('fileImport').onchange = (e) => { if (e.target.files[0]) importConfig(e.target.files[0]); e.target.value = ''; };

  $('btnKeyGen').onclick = () => {
    const b = crypto.getRandomValues(new Uint8Array(16));
    $('keyInput').value = [...b].map((x) => x.toString(16).padStart(2, '0')).join('');
  };
  $('btnKeySet').onclick = guard(async () => {
    const key = $('keyInput').value.trim().toLowerCase();
    if (!/^[0-9a-f]{32}$/.test(key)) return toast('Key must be exactly 32 hex characters.');
    await call('key.set', { key });
    await refreshInfo();
    toast('Key written and saved. Write the same key to the other board.');
  });

  $('btnK1').onclick = guard(() => relayTest(1));
  $('btnK2').onclick = guard(() => relayTest(2));
  $('btnPing').onclick = guard(() => call('radio.ping'));
  $('pingAuto').onchange = (e) => {
    clearInterval(pingTimer);
    if (e.target.checked) pingTimer = setInterval(() => call('radio.ping').catch(() => {}), 3000);
  };
  $('btnDiag').onclick = guard(async () => { $('diagOut').textContent = 'waiting for gate…'; await call('remote.diag'); });
  $('btnRemSet').onclick = guard(async () => {
    const name = $('remParam').value;
    const value = Number($('remValue').value);
    $('remResult').textContent = 'sending…';
    await call('remote.set', { name, value });
  });
  $('btnReplay').onclick = guard(() => call('debug.replay'));

  $('btnLogGet').onclick = guard(async () => {
    const res = await call('log.get');
    logLine(`--- board log (${res.log.length} entries, board uptime ${fmtDur(res.now)}) ---`);
    for (const e of res.log) logLine(`[${fmtDur(e.t)}] ${e.ev} a=${e.a} b=${e.b}`);
  });
  $('btnLogClear').onclick = () => { $('logView').innerHTML = ''; logLines.length = 0; };
  $('btnLogSave').onclick = () => download('gatelink-log.txt', logLines.join('\n'), 'text/plain');

  navigator.serial?.addEventListener('disconnect', (e) => { if (e.target === port) connectionLost(); });
  // A granted port reappearing while we're waiting for a reboot is the board coming back (the other
  // board never left, so it can't fire this).
  navigator.serial?.addEventListener('connect', (e) => {
    if (!reconnectUntil || port) return;
    lastPort = e.target;
    tryReconnect(lastPort);
  });
}

init();
