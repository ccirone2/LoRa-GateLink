// The site survey card: runs the survey and shows it; the verdict itself is survey.js's.
// Pings the other board for a few minutes, one ping at a time, and judges the margin both ways (the CLI's
// `gatelink.py survey` does the same). The firmware reports a pong only for its latest ping, so the ping card's
// buttons pause meanwhile, and auto-ping resumes after.
import { $, esc, cap } from './util.js';
import { S } from './state.js';
import { toast, guard } from './ui.js';
import { call } from './serial.js';
import { PING_TIMEOUT_MS, pausePing } from './ping.js';
import { analyze, noiseFromStatus, sampleFromPong, surveyText, tableRows, TABLE_HEAD } from './survey.js';

const SURVEY_INTERVAL_MS = 2000;
const NOISE_EVERY_MS = 10000; // the noise floors the samples are judged against are re-read this often
let survey = null; // the run in progress, or the last one (its results stay up, also after a disconnect)

const mmss = (ms) => `${Math.floor(ms / 60000)}:${String(Math.floor(ms / 1000) % 60).padStart(2, '0')}`;

function toggleSurvey() {
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
    noise: noiseFromStatus(status), noiseAt: Date.now(),
    pongWait: null, pongTimer: null, wake: null, napTimer: null, ticker: null, result: null,
    lastId: null, // the previous ping's ping_id, once a pong has shown the board's numbering
  };
  survey = r;
  pausePing(true);
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
  pausePing(false, resume);
  renderSurvey(r);
  const v = r.result.verdict ?? 'no result';
  if (!stopped) toast(`Site survey done: ${v}.`);
  else if (stopped === 'stopped') toast(`Site survey stopped early: ${v}.`);
  else toast(`Site survey ended early: ${stopped}.`, 'err');
}

function surveyControls(running) {
  $('surveyMins').disabled = running || !S.port;
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

async function copySurvey() {
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

// A pong event: the running survey's answer, if it waits for one.
export const surveyPong = (ev) => survey?.pongWait?.(ev);

// Another board connected: the last survey was of the other board's link.
export function resetSurvey() {
  if (survey) finishSurvey(survey, 'another board connected', false);
  survey = null;
  $('surveyBody').hidden = true;
  $('surveyStep').textContent = '';
  $('btnSurveyCopy').disabled = true;
}

// The board is going: end the run, keeping what it found.
export function stopSurvey() {
  if (survey) finishSurvey(survey, 'board disconnected', false);
}

export function initSurvey() {
  $('btnSurvey').onclick = guard(toggleSurvey);
  $('btnSurveyCopy').onclick = copySurvey;
}
