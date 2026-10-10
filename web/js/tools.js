// The Tools tab's radio and relay cards: ping and RSSI chart, the gate's diagnostics and remote writes, relay tests,
// the replay test.
import { $, fmtDur } from './util.js';
import { S } from './state.js';
import { toast } from './ui.js';
import { call } from './serial.js';
import { fmtFei } from './status.js';

const rssiHist = [];
// DIAG over LoRa is best effort; give up on the reply after this long.
const DIAG_TIMEOUT_MS = 10000;
let diagTimer = null;
// A ping is one unacknowledged frame each way; at SF12 the round trip takes a few seconds.
const PING_TIMEOUT_MS = 8000;
let pingWait = null;
let pingTimer = null;

export async function ping() {
  await call('radio.ping');
  if (pingWait) return; // timed from the oldest unanswered ping, or auto-ping would never time out
  if (!$('pingAuto').checked) $('pingRtt').textContent = 'waiting…';
  pingWait = setTimeout(() => {
    pingWait = null;
    $('pingRtt').textContent = 'no reply';
  }, PING_TIMEOUT_MS);
}

export function setAutoPing(on) {
  clearInterval(pingTimer);
  if (on) pingTimer = setInterval(() => ping().catch(() => {}), 3000);
}

function drawRssi() {
  const svg = $('rssiChart');
  if (!rssiHist.length) { svg.innerHTML = ''; $('rssiRange').textContent = ''; return; }
  const all = rssiHist.flatMap((p) => [p.here, p.peer]).filter(Number.isFinite);
  if (!all.length) { svg.innerHTML = ''; $('rssiRange').textContent = ''; return; }
  const lo = Math.min(...all) - 3, hi = Math.max(...all) + 3;
  const x = (i) => (rssiHist.length === 1 ? 150 : (i / (rssiHist.length - 1)) * 300);
  const y = (v) => 85 - ((v - lo) / (hi - lo || 1)) * 80;
  const line = (k, color) =>
    `<polyline stroke="var(${color})" points="${rssiHist.map((p, i) => (Number.isFinite(p[k]) ? `${x(i)},${y(p[k])}` : '')).join(' ')}"/>`;
  svg.innerHTML = line('here', '--chart-here') + line('peer', '--chart-peer');
  $('rssiRange').textContent = `${Math.round(lo + 3)} … ${Math.round(hi - 3)} dBm`;
}

// Results that belong to one board.
export function resetToolsView() {
  rssiHist.length = 0;
  drawRssi();
  for (const id of ['pingRtt', 'pingHere', 'pingPeer']) $(id).textContent = '—';
  $('diagOut').textContent = '—';
  $('remResult').textContent = '';
}

// The board is going: nothing more will answer these.
export function stopTools() {
  clearInterval(pingTimer);
  clearTimeout(diagTimer);
  clearTimeout(pingWait);
  pingWait = null;
  $('pingAuto').checked = false;
}

export async function relayTest(k) {
  const ms = Number($('relayMs').value);
  if (!Number.isInteger(ms) || ms < 50 || ms > 5000) return toast('Pulse length must be 50–5000 ms.', 'err');
  if (S.role === 'gate' && !confirm(`This will pulse the opener ${k === 1 ? 'OPEN' : 'CLOSE'} input and move the gate. Continue?`)) return;
  await call('relay.test', { k, ms });
  toast(`K${k} pulsed for ${ms} ms.`);
}

export async function remoteDiag() {
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
}

export async function remoteSet() {
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
}

export async function replay() {
  await call('debug.replay');
  toast('Replay sent. Check the other board’s replay counter (Status → Link).');
}

// The events these cards wait for.
export function onToolsEvent(ev) {
  switch (ev.event) {
    case 'pong':
      clearTimeout(pingWait);
      pingWait = null;
      $('pingRtt').textContent = `${ev.rtt_ms} ms`;
      $('pingHere').textContent = `${ev.rssi} dBm / ${Number(ev.snr).toFixed(1)} dB`;
      $('pingPeer').textContent = ev.peer_rssi === undefined ? '—' : `${ev.peer_rssi} dBm / ${Number(ev.peer_snr).toFixed(1)} dB`;
      $('pingFei').textContent = fmtFei(ev.fei);
      rssiHist.push({ here: Number(ev.rssi), peer: Number(ev.peer_rssi) }); // NaN = missing, left out of the chart
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
