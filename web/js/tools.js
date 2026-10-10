// The Tools tab's radio and relay cards: ping and RSSI chart, the site survey, the gate's diagnostics and remote
// writes, relay tests, the replay test.
import { $, esc, cap, fmtDur } from './util.js';
import { S } from './state.js';
import { toast } from './ui.js';
import { call } from './serial.js';
import { fmtFei } from './status.js';
import { analyze, noiseFromStatus, sampleFromPong, surveyText, tableRows, TABLE_HEAD } from './survey.js';

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

// ---------- Site survey ----------
// Pings the other board for a few minutes, one ping at a time, and judges the margin both ways (survey.js; the CLI's
// `gatelink.py survey` does the same). The firmware reports a pong only for its latest ping, so the ping card's
// buttons pause meanwhile, and auto-ping resumes after.
const SURVEY_INTERVAL_MS = 2000;
const NOISE_EVERY_MS = 10000; // the noise floors the samples are judged against are re-read this often
let survey = null; // the run in progress, or the last one (its results stay up, also after a disconnect)

const mmss = (ms) => `${Math.floor(ms / 60000)}:${String(Math.floor(ms / 1000) % 60).padStart(2, '0')}`;

export function toggleSurvey() {
  return survey?.running ? finishSurvey(survey, 'stopped') : startSurvey();
}

async function startSurvey() {
  const { status } = await call('status');
  if (!status.link?.verified) throw new Error('Survey needs the link up: the other board powered, with the same key and radio settings.');
  if (S.params.sf === undefined) throw new Error('Settings not loaded yet: Config → Reload from board, then start again.');
  const r = {
    running: true, role: S.role, fw: S.boardInfo?.fw ?? '?', time: new Date(), samples: [], stopped: null,
    settings: { sf: S.params.sf, bw_hz: S.params.bw_hz, tx_power: S.params.tx_power },
    durationMs: Number($('surveyMins').value) * 60000, startedAt: Date.now(), elapsedMs: 0,
    noise: noiseFromStatus(status), noiseAt: Date.now(), autoPing: $('pingAuto').checked,
    pongWait: null, pongTimer: null, wake: null, napTimer: null, ticker: null, result: null,
    lastId: null, // the previous ping's ping_id, once a pong has shown the board's numbering
  };
  survey = r;
  if (r.autoPing) {
    $('pingAuto').checked = false;
    setAutoPing(false);
  }
  surveyControls(true);
  r.ticker = setInterval(() => surveyProgress(r), 1000);
  renderSurvey(r);
  runSurvey(r).catch((e) => finishSurvey(r, `failed: ${e.message}`));
}

async function runSurvey(r) {
  while (r.running && Date.now() - r.startedAt < r.durationMs) {
    const start = Date.now();
    if (start - r.noiseAt >= NOISE_EVERY_MS) {
      r.noiseAt = start;
      try {
        r.noise = noiseFromStatus((await call('status')).status);
      } catch { /* keep the last floors */ }
      if (!r.running) break;
    }
    // The previous ping's pong can still land after its 8 s, just before this ping reaches the board. It isn't this
    // ping's answer, so it is skipped by its id.
    const stale = r.lastId;
    const answer = new Promise((resolve) => {
      r.pongWait = (ev) => {
        if (ev && stale !== null && ev.ping_id === stale) return;
        clearTimeout(r.pongTimer);
        r.pongWait = null;
        resolve(ev);
      };
      r.pongTimer = setTimeout(() => r.pongWait?.(null), PING_TIMEOUT_MS);
    });
    await call('radio.ping');
    const pong = await answer;
    if (!r.running) break; // stopped meanwhile: that ping doesn't count
    // This ping's id: its pong's, else one past the previous ping's (the board counts every radio.ping).
    r.lastId = Number.isInteger(pong?.ping_id) ? pong.ping_id : stale === null ? null : (stale + 1) & 0xffff;
    r.samples.push(sampleFromPong(pong, (start - r.startedAt) / 1000, r.noise));
    renderSurvey(r);
    const next = start + SURVEY_INTERVAL_MS;
    if (next - r.startedAt >= r.durationMs) break;
    await new Promise((resolve) => {
      r.wake = resolve;
      r.napTimer = setTimeout(resolve, Math.max(0, next - Date.now()));
    });
  }
  finishSurvey(r, null);
}

// Ends the run (idempotent). `stopped` says why it ended early; with `resume`, auto-ping comes back on if it was.
function finishSurvey(r, stopped, resume = true) {
  if (!r.running) return;
  r.running = false;
  r.stopped = stopped;
  r.elapsedMs = Date.now() - r.startedAt;
  clearInterval(r.ticker);
  clearTimeout(r.napTimer);
  r.wake?.();
  r.pongWait?.(null);
  surveyControls(false);
  if (resume && r.autoPing && S.port) {
    $('pingAuto').checked = true;
    setAutoPing(true);
  }
  renderSurvey(r);
  const v = r.result.verdict ?? 'no result';
  if (!stopped) toast(`Site survey done: ${v}.`);
  else if (stopped === 'stopped') toast(`Site survey stopped early: ${v}.`);
  else toast(`Site survey ended early: ${stopped}.`, 'err');
}

function surveyControls(running) {
  for (const id of ['btnPing', 'pingAuto', 'surveyMins']) $(id).disabled = running || !S.port;
  $('btnSurvey').textContent = running ? 'Stop survey' : 'Start survey';
  $('btnSurvey').classList.toggle('primary', !running);
  $('surveyProgress').hidden = !running;
}

function surveyProgress(r) {
  const s = r.result?.summary;
  const counts = s ? `${s.sent} ping${s.sent === 1 ? '' : 's'}, ${s.lost} unanswered` : 'no pings yet';
  if (r.running) {
    const ms = Math.min(Date.now() - r.startedAt, r.durationMs);
    $('surveyProgress').value = ms / r.durationMs;
    $('surveyStep').textContent = `Surveying from the ${r.role} board: ${mmss(ms)} of ${mmss(r.durationMs)} · ${counts}`;
  } else {
    $('surveyStep').textContent = `${r.stopped ? cap(r.stopped) : 'Done'} after ${mmss(r.elapsedMs)}: ${counts}. `
      + `Started ${r.time.toLocaleTimeString()} on the ${r.role} board.`;
  }
}

function renderSurvey(r) {
  const a = analyze(r.settings, r.samples, r.role);
  r.result = a;
  $('surveyBody').hidden = !r.samples.length;
  $('btnSurveyCopy').disabled = r.running || !r.samples.length;
  surveyProgress(r);
  if (!r.samples.length) return;
  $('surveyVerdict').className = `verdict ${a.verdict ?? ''}`;
  $('surveyVerdict').textContent = a.verdict ? `${cap(a.verdict)}${r.running ? ' so far' : ''}` : '—';
  // The headline starts with the verdict, which the big word before it already says.
  $('surveyHeadline').textContent = a.headline.replace(/^\w+: /, '');
  $('surveyTable').innerHTML = `<thead><tr>${TABLE_HEAD.map((h) => `<th>${esc(h)}</th>`).join('')}</tr></thead><tbody>`
    + tableRows(r.role, a.summary).map((row) => `<tr>${row.map((c) => `<td>${esc(c)}</td>`).join('')}</tr>`).join('')
    + '</tbody>';
  // Advice once it's over: a verdict so far can still change.
  $('surveyAdvice').innerHTML = r.running ? '' : a.advice.map((t) => `<li>${esc(t)}</li>`).join('');
}

export async function copySurvey() {
  const r = survey;
  if (!r || r.running || !r.samples.length) return toast('Nothing to copy yet: run a survey first.', 'err');
  const text = surveyText({ time: r.time, board: { role: r.role, fw: r.fw }, settings: r.settings,
    elapsed_s: r.elapsedMs / 1000, stopped: r.stopped, ...r.result });
  try {
    await navigator.clipboard.writeText(text);
    toast('Survey report copied to the clipboard.');
  } catch (e) {
    toast(`Copy failed: ${e.message}`, 'err');
  }
}

// Results that belong to one board.
export function resetToolsView() {
  rssiHist.length = 0;
  drawRssi();
  for (const id of ['pingRtt', 'pingHere', 'pingPeer']) $(id).textContent = '—';
  $('diagOut').textContent = '—';
  $('remResult').textContent = '';
  if (survey) finishSurvey(survey, 'another board connected', false);
  survey = null;
  $('surveyBody').hidden = true;
  $('surveyStep').textContent = '';
  $('btnSurveyCopy').disabled = true;
}

// The board is going: nothing more will answer these.
export function stopTools() {
  if (survey) finishSurvey(survey, 'board disconnected', false);
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
      survey?.pongWait?.(ev);
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
