// The other board, as this page last saw it (ROADMAP F10). The settings that must match (MUST_MATCH) are exactly the
// ones that keep the link from verifying when they differ, so the boards can't compare them over the air: the page
// remembers each board's firmware, key id and must-match settings by role whenever it is connected, and with the
// other one connected says what differs. Connected to the house with the link up, it also asks the gate for its
// firmware (remote.diag). Kept in this browser only (localStorage): a convenience, never needed. Boards are told apart
// by role alone, so the other board is whichever this browser saw last in that role (the line says how long ago).
import { esc, fmtDur } from './util.js';
import { S } from './state.js';
import { call } from './serial.js';
import { MUST_MATCH } from './settings.js';

const STORE = 'gatelink.boards';
const ROLES = ['house', 'gate'];

let cache = null; // the parsed store: read once, kept in step by save()

function load() {
  if (cache) return cache;
  try {
    const v = JSON.parse(localStorage.getItem(STORE));
    cache = v && typeof v === 'object' ? v : {};
  } catch {
    cache = {};
  }
  return cache;
}

function save(all) {
  cache = all;
  try {
    localStorage.setItem(STORE, JSON.stringify(all));
  } catch { /* storage blocked (a private window): kept for this page only */ }
}

// Compared only once the connected board's own info and config are in (rememberBoard), not while S still holds the
// previous board's.
let ready = false;

// The connected board, once its info and config are in: after loadConfig, and again after its key changes.
export function rememberBoard() {
  if (!S.boardInfo || !ROLES.includes(S.role)) return;
  ready = true;
  const all = { ...load() };
  const params = Object.fromEntries([...MUST_MATCH].filter((n) => n in S.params).map((n) => [n, S.params[n]]));
  all[S.role] = { fw: S.boardInfo.fw, key_id: S.boardInfo.key_id, params, at: Date.now() };
  save(all);
}

// The house's question to the gate: DIAG_REQ isn't retried by the link, so ask again (up to 3 times, 30 s apart) while
// no answer has come.
const ASK_EVERY_MS = 30000;
let askedAt = 0, asks = 0, gotGateFw = false;
export const resetPeer = () => { ready = false; askedAt = 0; asks = 0; gotGateFw = false; };

// The gate's firmware from a remote_diag reply (the house's link to it): only the version, the rest kept as seen.
export function rememberGateFw(fw) {
  if (S.role !== 'house' || !fw) return;
  gotGateFw = true;
  save({ ...load(), gate: { ...(load().gate || {}), fw, fwAt: Date.now() } });
}

// Connected to the house with its link up: the gate's firmware, without a trip to the gate.
export function askGateFw(status) {
  if (gotGateFw || asks >= 3 || status.role !== 'house' || !status.link_up) return;
  if (asks && Date.now() - askedAt < ASK_EVERY_MS) return;
  asks++;
  askedAt = Date.now();
  call('remote.diag').catch(() => {});
}

const minor = (v) => String(v).split('.').slice(0, 2).join('.');
const PARAM_TEXT = { freq_hz: (v) => `${v / 1e6} MHz`, bw_hz: (v) => `${v / 1000} kHz`, cr: (v) => `4/${v}`,
  sync_word: (v) => `0x${Number(v).toString(16)}` };
const show = (name, v) => (PARAM_TEXT[name] ? PARAM_TEXT[name](v) : String(v));

// What differs between the connected board and the other one as last seen: { other, there, diffs, fwMinor }, with
// other null before the other board has been seen. diffs: [{ name, here, there }] (key id and must-match settings);
// fwMinor: the firmware differs in MAJOR.MINOR, a change both boards need together (docs/development.md).
export function peerDiffs() {
  const other = S.role === 'house' ? 'gate' : S.role === 'gate' ? 'house' : null;
  const there = other ? load()[other] : null;
  if (!there || !S.boardInfo || !ready) return { other, there: null, diffs: [], fwMinor: false };
  const diffs = [];
  // key_id: a string, null with no key set, absent before firmware 0.13.8 (not compared then).
  const here = S.boardInfo.key_id, theirs = there.key_id;
  if (here !== undefined && theirs !== undefined && here !== theirs) diffs.push({ name: 'key id', here: here ?? 'none', there: theirs ?? 'none' });
  for (const [name, v] of Object.entries(there.params || {})) {
    if (name in S.params && S.params[name] !== v) diffs.push({ name, here: show(name, S.params[name]), there: show(name, v) });
  }
  const fwMinor = !!there.fw && !!S.boardInfo.fw && minor(there.fw) !== minor(S.boardInfo.fw);
  return { other, there, diffs, fwMinor };
}

// The Status card's line: what differs from the other board, and how old that view is.
export function peerHtml(verified, pd = peerDiffs()) {
  const { other, there, diffs, fwMinor } = pd;
  if (!other || !ready) return '—';
  if (!there) return `${other} not seen yet: connect it once to compare`;
  if (!there.params) { // only the gate's firmware, from the house's remote.diag
    const fw = `${other} fw ${esc(there.fw)}`;
    return fwMinor ? `<span class="bad">firmware ${esc(S.boardInfo.fw)} here, ${esc(there.fw)} on the ${other}: update both</span>`
      : `${fw}; its settings not seen yet: connect it once to compare`;
  }
  const parts = diffs.map((d) => `${esc(d.name)} ${esc(d.here)} here, ${esc(d.there)} on the ${other}`);
  if (fwMinor) parts.push(`firmware ${esc(S.boardInfo.fw)} here, ${esc(there.fw)} on the ${other}: update both`);
  const ago = there.at ? ` · ${other} seen ${fmtDur(Date.now() - there.at)} ago` : '';
  if (!parts.length) return `<span class="good">matches</span>${there.fw ? ` · ${other} fw ${esc(there.fw)}` : ''}${ago}`;
  const lead = !verified && diffs.length ? 'the link can’t verify, these differ: ' : 'differs: ';
  return `<span class="bad">${lead}${parts.join('; ')}</span>${ago}`;
}
