// Field wiring per board: the Install tab's diagram, terminal table and notes, and the status card's I/O labels.
// Keep in step with firmware/GateLink/pins.h, the role behaviour and docs/hardware.md.
import { $, esc, wrapText } from './util.js';

export const IO_LABELS = {
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
// D5, the "needs attention" output (fault_out), on either board: a logic output (kind out), not a relay contact.
const FAULT_OUTPUT = { name: 'Fault output (optional)', hint: 'D5 high = healthy · needs fault_out on', kind: 'out',
  rows: [['FAULT (D5)', 'Relay module IN (active high)'], ['VIN (5 V)', 'Relay module VCC'], ['GND', 'Relay module GND']] };
const FAULT_NOTE = 'D5 is an optional “needs attention” output for the alarm system (<code>fault_out</code>, off by default): high while the board is healthy, low once a problem has lasted <code>fault_hold_s</code>, and low after a restart until the board has started up. It is a 3.3 V logic output good for a few mA: drive a relay module with an active-high input that switches at 3.3 V (or an opto), never a relay coil directly. Wire the module’s NO and COM contacts to a second contact sensor’s terminals (or a hardwired zone) as a normally-closed loop: closed while healthy, open on a problem, a dead board or a cut wire. With <code>fault_out</code> off, D5 stays an input; leave it unwired.';
export const WIRING = {
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
      FAULT_OUTPUT,
      { name: '5 V supply', hint: 'Or USB', kind: 'pwr',
        rows: [['VIN (5 V)', '+5 V'], ['GND', '0 V']] },
    ],
    notes: [
      'IN1 reads the controller’s relay contact switched to the board’s 3.3 V (internal pull-down; open = off). The controller output must be a potential-free contact, and nothing above 3.3 V may reach IN1. If ON and OFF come out reversed, fix it in the wiring (use the other relay contact, or change the controller’s output mode), never with <code>in1_invert</code>: inverted, a cut wire would read as ON. Keep all <code>inN_invert</code> at 0.',
      'K1 mirrors the real gate back to the controller so its switch always shows the true state. Set the controller’s switch input to toggle/follow mode (contact closed = ON, open = OFF), not detached. Wire it per the controller’s switch-input diagram. Low voltage only; never switch mains with the shield.',
      'IN2 senses the controller’s supply through a PNP-output opto channel wired across it (use a channel rated for that voltage; output side from 3.3 V only). When the controller loses power its relay drops, which would otherwise look like a user turning the switch off: while IN2 is off (or, with <code>ctrl_power_pmic</code>, the board’s own supply, which shares the controller’s), controller edges are logged but never sent; a switch-off (CLOSE) waits <code>ctrl_confirm_ms</code> in case power is failing, while a switch-on (OPEN) goes at once; and after power returns its edges count as sync for at least <code>ctrl_settle_ms</code>. Set <code>ctrl_power_sense</code> to 0 if IN2 isn’t wired.',
      'The contact sensor needs an external terminal input. K2 closes when the gate is closed and opens if the link is lost (<code>linkloss_open</code>). <code>sensor_invert</code> flips it.',
      'VIN is 5 V max. Feed it from the controller’s supply through a 5 V converter, so the board’s supply sense (<code>ctrl_power_pmic</code>) sees the controller’s power. USB power is fine for setup, but a USB cable that carries power keeps that sense reading ok.',
      SPARE_NOTE,
      FAULT_NOTE,
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
      FAULT_OUTPUT,
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
      FAULT_NOTE,
    ],
  },
};

let wiringShown = 'house';

// Two layouts: side by side, or a compact one for phones that fits ~360 px without scrolling (text wraps).
const WIRING_LAYOUT = {
  wide: { bx: 16, bw: 250, dx: 500, dw: 250, width: 766, rowH: 28, title: 30, hint: 38, sub: 40 },
  narrow: { bx: 4, bw: 118, dx: 150, dw: 206, width: 360, rowH: 26, title: 23, hint: 30, sub: 16 },
};
let wiringNarrow = false;

export function renderWiring(r) {
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
  svg.setAttribute('aria-label', `${r === 'house' ? 'House' : 'Gate'} board field wiring diagram (the table below lists the same connections)`);
  svg.innerHTML = board + blocks + wires;

  $('wiringTable').innerHTML = '<thead><tr><th>Device</th><th>Board terminal</th><th>Device terminal</th></tr></thead><tbody>'
    + W.groups.map((g) => g.rows.map(([bt, dt], i) =>
      `<tr>${i === 0 ? `<td class="dev" rowspan="${g.rows.length}">${esc(g.name)}<small>${esc(g.hint)}</small></td>` : ''}`
      + `<td class="mono">${esc(bt)}</td><td class="mono">${esc(dt)}</td></tr>`).join('')).join('')
    + '</tbody>';
  $('wiringNotes').innerHTML = W.notes.map((n) => `<li>${n}</li>`).join('');
}

// The Install tab resized: switch between the wide and the phone layout when crossing 600 px.
export function wiringResized(width) {
  if (!width || (width < 600) === wiringNarrow) return; // 0 = Install tab hidden
  wiringNarrow = width < 600;
  renderWiring(wiringShown);
}

export const wiringRole = () => wiringShown;
