// The link ping card: one ping, or one every 3 s; the round trip, both ends' signal, the margin (for aiming an
// antenna: it changes with each pong) and an RSSI chart.
import { $, fmtFei } from './util.js';
import { fmt1, marginLevel, sampleMargin } from './survey.js';
import { S } from './state.js';
import { guard } from './ui.js';
import { call } from './serial.js';

// A ping is one unacknowledged frame each way; at SF12 the round trip takes a few seconds.
export const PING_TIMEOUT_MS = 8000;
const rssiHist = [];
let pingWait = null;
let pingTimer = null;

async function ping() {
  await call('radio.ping');
  if (pingWait) return; // timed from the oldest unanswered ping, or auto-ping would never time out
  if (!$('pingAuto').checked) $('pingRtt').textContent = 'waiting…';
  pingWait = setTimeout(() => {
    pingWait = null;
    $('pingRtt').textContent = 'no reply';
  }, PING_TIMEOUT_MS);
}

function setAutoPing(on) {
  clearInterval(pingTimer);
  if (on) pingTimer = setInterval(() => ping().catch(() => {}), 3000);
}

// The site survey pings meanwhile (the board reports a pong only for its latest ping): pausing disables the card's
// controls (through their fieldset, so a ping still in flight can't re-enable its button) and stops auto-ping;
// unpausing with `resume` turns auto-ping back on if it was.
let resumeAuto = false;
export function pausePing(on, resume = true) {
  $('pingControls').disabled = on;
  if (on) {
    resumeAuto = $('pingAuto').checked;
    $('pingAuto').checked = false;
    setAutoPing(false);
    return;
  }
  if (resume && resumeAuto) {
    $('pingAuto').checked = true;
    setAutoPing(true);
  }
  resumeAuto = false;
}

// Each direction's margin above the SF's floor, coloured by the survey's thresholds; '—' for a direction not heard.
function marginHtml(ev) {
  const { noise, peer_noise: peerNoise } = S.noise;
  const m = (snr, rssi, n) => (snr === undefined || snr === null ? null : sampleMargin(S.params.sf, Number(snr), Number(rssi), n));
  const show = (x) => (x === null ? '—' : `<span class="lvl-${marginLevel(x)}">${fmt1(x)}</span>`);
  return `${show(m(ev.snr, ev.rssi, noise))} / ${show(m(ev.peer_snr, ev.peer_rssi, peerNoise))} dB`;
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

// A pong event: the board's answer to its latest ping.
export function onPong(ev) {
  clearTimeout(pingWait);
  pingWait = null;
  $('pingRtt').textContent = `${ev.rtt_ms} ms`;
  $('pingHere').textContent = `${ev.rssi} dBm / ${Number(ev.snr).toFixed(1)} dB`;
  $('pingPeer').textContent = ev.peer_rssi === undefined ? '—' : `${ev.peer_rssi} dBm / ${Number(ev.peer_snr).toFixed(1)} dB`;
  $('pingMargin').innerHTML = marginHtml(ev);
  $('pingFei').textContent = fmtFei(ev.fei, S.params.freq_hz);
  rssiHist.push({ here: Number(ev.rssi), peer: Number(ev.peer_rssi) }); // NaN = missing, left out of the chart
  if (rssiHist.length > 60) rssiHist.shift();
  drawRssi();
}

// Another board connected: these results were the last one's.
export function resetPing() {
  rssiHist.length = 0;
  drawRssi();
  for (const id of ['pingRtt', 'pingHere', 'pingPeer', 'pingMargin', 'pingFei']) $(id).textContent = '—';
}

// The board is going: nothing more will answer.
export function stopPing() {
  clearInterval(pingTimer);
  clearTimeout(pingWait);
  pingWait = null;
  $('pingAuto').checked = false;
  resumeAuto = false;
}

export function initPing() {
  $('btnPing').onclick = guard(ping);
  $('pingAuto').onchange = (e) => setAutoPing(e.target.checked);
}
