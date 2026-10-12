// The other board, as this page last saw it (ROADMAP F10). The settings that must match (MUST_MATCH) are exactly the
// ones that keep the link from verifying when they differ, so the boards can't compare them over the air: the page
// remembers each board's firmware, key id and must-match settings by role whenever it is connected, and with the
// other one connected says what differs. Connected to the house with the link up, it also asks the gate for its
// firmware (remote.diag). Kept in this browser only (localStorage): a convenience, never needed.
import { esc, fmtDur } from './util.js';
import { S } from './state.js';
import { call } from './serial.js';
import { MUST_MATCH } from './settings.js';

const STORE = 'gatelink.boards';
const ROLES = ['house', 'gate'];

function load() {
  try {
    const v = JSON.parse(localStorage.getItem(STORE));
    return v && typeof v === 'object' ? v : {};
  } catch {
    return {};
  }
}

function save(all) {
  try {
    localStorage.setItem(STORE, JSON.stringify(all));
  } catch { /* storage blocked (a private window): nothing to compare against next time */ }
}

// The connected board, once its info and config are in (after loadConfig).
export function rememberBoard() {
  if (!S.boardInfo || !ROLES.includes(S.role)) return;
  const all = load();
  const params = Object.fromEntries([...MUST_MATCH].filter((n) => n in S.params).map((n) => [n, S.params[n]]));
  all[S.role] = { fw: S.boardInfo.fw, key_id: S.boardInfo.key_id, params, at: Date.now() };
  save(all);
}

// The gate's firmware from a remote_diag reply (the house's link to it): only the version, the rest kept as seen.
export function rememberGateFw(fw) {
  if (S.role !== 'house' || !fw) return;
  const all = load();
  all.gate = { ...(all.gate || {}), fw, fwAt: Date.now() };
  save(all);
}

let askedGate = false;
export const resetPeer = () => { askedGate = false; };

// Once per connection to the house, when its link is up: the gate's firmware, without a trip to the gate.
export function askGateFw(status) {
  if (askedGate || status.role !== 'house' || !status.link_up) return;
  askedGate = true;
  call('remote.diag').catch(() => { askedGate = false; });
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
  if (!there || !S.boardInfo) return { other, there: null, diffs: [], fwMinor: false };
  const diffs = [];
  const here = S.boardInfo.key_id, theirs = there.key_id;
  if (typeof here === 'string' && typeof theirs === 'string' && here !== theirs) diffs.push({ name: 'key id', here, there: theirs });
  for (const [name, v] of Object.entries(there.params || {})) {
    if (name in S.params && S.params[name] !== v) diffs.push({ name, here: show(name, S.params[name]), there: show(name, v) });
  }
  const fwMinor = !!there.fw && !!S.boardInfo.fw && minor(there.fw) !== minor(S.boardInfo.fw);
  return { other, there, diffs, fwMinor };
}

// The Status card's line: what differs from the other board, and how old that view is.
export function peerHtml(verified) {
  const { other, there, diffs, fwMinor } = peerDiffs();
  if (!other) return '—';
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
