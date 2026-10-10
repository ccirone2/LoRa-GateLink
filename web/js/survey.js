// Site survey: how much margin the radio link has, judged from pings (radio.ping, answered by pong events). Pure
// functions, no page: the Tools tab's survey card runs them (tools.js), and tools/gatelink_client/survey.py is the
// same logic for `tools/gatelink.py survey`. Both are checked against tests/tools/fixtures/survey-vectors.json, so
// change them together. Texts here are plain ASCII, as the command line prints the same ones.

// SX1276 demodulation SNR floor per spreading factor (Semtech SX1276 datasheet), dB: below it a frame is lost.
export const SNR_FLOOR = { 6: -5, 7: -7.5, 8: -10, 9: -12.5, 10: -15, 11: -17.5, 12: -20 };
export const GOOD_DB = 15; // the weaker direction's 10th-percentile margin for "good"
export const FAIR_DB = 10; // ... for "fair"; below it, "marginal"
// More pings unanswered than MAX_LOSS, and at least MIN_LOST of them, is "poor" whatever the margin: one lost ping in
// a short survey is chance (the bench house misses a few % of pongs), a steady loss is not. Above NOTE_LOSS the advice
// says frames are being lost.
export const MAX_LOSS = 0.05;
export const MIN_LOST = 2;
export const NOTE_LOSS = 0.02;
// The SNR reading saturates near +10 dB on a strong link: from here up, RSSI minus the noise floor estimates it better.
export const SNR_SATURATES_DB = 5;
export const TX_POWER_MIN = 2; // tx_power's range in config.cpp PARAMS, dBm
export const TX_POWER_MAX = 20;
export const SF_MAX = 12;
export const FEW_PONGS = 20; // fewer answered pings than this: say the result is a rough one
// `here` is this board's reception of the pongs, `peer` the other board's reception of the pings.
export const DIRECTIONS = ['here', 'peer'];
const FIELDS = { here: ['rssi', 'snr', 'noise'], peer: ['peer_rssi', 'peer_snr', 'peer_noise'] };

const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
const minOf = (a) => (a.length ? Math.min(...a) : null);

export const snrFloor = (sf) => SNR_FLOOR[sf] ?? null;

// One decimal, ties rounded up: the same digits in Python (toFixed alone rounds 0.25 the other way there).
export const fmt1 = (x) => (Math.floor(x * 10 + 0.5) / 10).toFixed(1);

export const otherRole = (role) => (role === 'house' ? 'gate' : role === 'gate' ? 'house' : 'other board');
export const dirLabel = (role, dir) => `at the ${dir === 'here' ? role : otherRole(role)}`;

// dB above the SF's demodulation floor for one received frame. RSSI minus the noise floor stands in for the SNR
// only when the SNR reads 5 dB or more and the noise floor is known.
export function sampleMargin(sf, snr, rssi, noise) {
  const floor = snrFloor(sf);
  if (floor === null || !isNum(snr)) return null;
  const bySnr = snr - floor;
  if (snr < SNR_SATURATES_DB || !isNum(noise) || !isNum(rssi)) return bySnr;
  return Math.max(bySnr, rssi - noise - floor);
}

// Linear interpolation between the closest ranks (numpy's default): q = 0.5 is the median.
export function quantile(values, q) {
  const s = values.filter(isNum).sort((a, b) => a - b);
  if (!s.length) return null;
  const pos = q * (s.length - 1);
  const lo = Math.floor(pos), hi = Math.ceil(pos);
  return s[lo] + (s[hi] - s[lo]) * (pos - lo);
}

// The noise floors a sample is judged against, from a status reply: this board's, and on the house the gate's.
export function noiseFromStatus(s) {
  const noise = s?.link?.noise;
  const peer = s?.role === 'house' ? s?.remote?.noise : null;
  return { noise: isNum(noise) ? noise : null, peer_noise: isNum(peer) ? peer : null };
}

// A survey sample: a pong (or null: no answer in time) at `t` seconds into the survey, with the noise floors then.
export function sampleFromPong(pong, t, { noise = null, peer_noise = null } = {}) {
  if (!pong) return { t, lost: true, noise, peer_noise };
  const v = (k) => (isNum(pong[k]) ? pong[k] : null);
  return { t, lost: false, rtt_ms: v('rtt_ms'), rssi: v('rssi'), snr: v('snr'), peer_rssi: v('peer_rssi'),
    peer_snr: v('peer_snr'), fei: v('fei'), noise, peer_noise };
}

export function summarizeDirection(sf, samples, dir) {
  const [kr, ks, kn] = FIELDS[dir];
  const got = samples.filter((s) => !s.lost && isNum(s[ks]));
  const rssi = got.map((s) => s[kr]).filter(isNum);
  const snr = got.map((s) => s[ks]);
  const margin = got.map((s) => sampleMargin(sf, s[ks], s[kr], s[kn])).filter(isNum);
  return {
    n: got.length, rssi_min: minOf(rssi), rssi_median: quantile(rssi, 0.5), snr_min: minOf(snr),
    snr_median: quantile(snr, 0.5), margin_min: minOf(margin), margin_p10: quantile(margin, 0.1),
  };
}

// Both directions, the loss, and the weaker direction (the lower 10th-percentile margin; `here` on a tie).
export function summarize(settings, samples) {
  const sent = samples.length;
  const lost = samples.filter((s) => s.lost).length;
  const out = { sent, answered: sent - lost, lost, loss: sent ? lost / sent : null };
  for (const d of DIRECTIONS) out[d] = summarizeDirection(settings.sf, samples, d);
  let worst = null;
  for (const d of DIRECTIONS) {
    const p = out[d].margin_p10;
    if (p !== null && (worst === null || p < out[worst].margin_p10)) worst = d;
  }
  out.worst = worst;
  out.worst_p10 = worst === null ? null : out[worst].margin_p10;
  return out;
}

// Enough pings went unanswered to call the link poor.
export function tooLossy(summary) {
  return summary.lost >= MIN_LOST && summary.loss > MAX_LOSS;
}

// good / fair / marginal from the weaker direction's 10th-percentile margin; poor if too many pings went
// unanswered; null if nothing was sent.
export function verdict(summary) {
  if (!summary.sent) return null;
  if (tooLossy(summary) || summary.worst_p10 === null) return 'poor';
  if (summary.worst_p10 >= GOOD_DB) return 'good';
  if (summary.worst_p10 >= FAIR_DB) return 'fair';
  return 'marginal';
}

// One line saying what the verdict rests on.
export function headline(role, settings, summary, v) {
  if (v === null) return 'No result: no pings were sent.';
  const name = v[0].toUpperCase() + v.slice(1);
  const margin = summary.worst === null ? null
    : `the weaker direction (${dirLabel(role, summary.worst)}) keeps ${fmt1(summary.worst_p10)} dB above the SF${settings.sf} limit in 90 % of pings`;
  if (tooLossy(summary)) {
    return `${name}: ${fmt1(100 * summary.loss)} % of pings went unanswered (more than ${fmt1(100 * MAX_LOSS)} %)`
      + (margin ? `, though ${margin}.` : '.');
  }
  if (margin === null) return `${name}: no answer carried a signal report.`;
  return `${name}: ${margin} (good from ${GOOD_DB} dB, fair from ${FAIR_DB} dB).`;
}

// What to do about it, in plain words.
export function advice(settings, summary, v) {
  if (v === null) return ['Run the survey with the link up.'];
  const tx = isNum(settings.tx_power) ? settings.tx_power : null;
  const sf = isNum(settings.sf) ? settings.sf : null;
  const out = [];
  if (summary.answered > 0 && summary.answered < FEW_PONGS) {
    out.push(`Only ${summary.answered} pings were answered: run the survey longer for a firmer result.`);
  }
  if (summary.loss > NOTE_LOSS) {
    out.push('Frames are being lost: check the antennas and their cables, and both boards\' noise floor for interference.');
  }
  if (v === 'good') {
    const room = tx === null ? 0 : Math.min(Math.floor(summary.worst_p10 - GOOD_DB), tx - TX_POWER_MIN);
    out.push(room >= 1
      ? `Plenty of margin. If you like, tx_power (${tx} dBm on this board) could be lowered by up to ${room} dB on both boards and still keep ${GOOD_DB} dB.`
      : 'Plenty of margin: nothing to change.');
  } else if (v === 'fair') {
    out.push(`Usable, with less than ${GOOD_DB} dB in hand for rain, foliage and interference. If it is easy, mount the antennas higher with a clearer line of sight between the boards, and survey again.`);
  } else {
    out.push('Mount the antennas higher, clear of metal, walls and the relay shield, with as clear a line of sight between the boards as you can get: that gains more than any setting.');
    if (tx !== null && tx < TX_POWER_MAX) {
      out.push(`Raise tx_power (${tx} dBm on this board, at most ${TX_POWER_MAX}) on both boards, each on a solid supply: a weak one can reset the board at full power.`);
    }
    if (sf !== null && sf < SF_MAX) {
      out.push(`Or raise sf (now ${sf}) on BOTH boards, as radio settings must match or the link stops: each step adds about 2.5 dB of margin and roughly doubles the airtime.`);
    }
    if (tx !== null && sf !== null && tx >= TX_POWER_MAX && sf >= SF_MAX) {
      out.push('tx_power and sf are at their limits: only antenna placement, or a higher-gain antenna, can help.');
    }
    out.push('Then run the survey again.');
  }
  return out;
}

// Everything the survey says about these samples.
export function analyze(settings, samples, role) {
  const summary = summarize(settings, samples);
  const v = verdict(summary);
  return { summary, verdict: v, headline: headline(role, settings, summary, v), advice: advice(settings, summary, v) };
}

const num = (x, unit = '') => (isNum(x) ? `${fmt1(x)}${unit}` : '-');
const pair = (a, b, unit) => `${num(a)} / ${num(b)}${unit}`;
const mmss = (s) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, '0')}`;

// The rows of the per-direction table: [label, received, RSSI, SNR, margin].
export function tableRows(role, summary) {
  return DIRECTIONS.map((d) => {
    const x = summary[d];
    return [dirLabel(role, d), String(x.n), pair(x.rssi_min, x.rssi_median, ' dBm'), pair(x.snr_min, x.snr_median, ' dB'),
      pair(x.margin_min, x.margin_p10, ' dB')];
  });
}
export const TABLE_HEAD = ['Direction', 'Received', 'RSSI min / median', 'SNR min / median', 'Margin min / 10th pct'];

// The plain-text report (Copy report): report = { time (Date), board: { role, fw }, settings, elapsed_s, stopped,
// summary, headline, advice }.
export function surveyText(r) {
  const s = r.summary, bw = r.settings.bw_hz;
  const lines = [
    `GateLink site survey, ${r.time.toLocaleString()}`,
    `Board: ${r.board.role}, firmware ${r.board.fw}`,
    `Radio: SF${r.settings.sf}, ${isNum(bw) ? `${bw / 1000} kHz` : '?'}, tx_power ${r.settings.tx_power} dBm on this board`,
    `Ran ${mmss(r.elapsed_s)}${r.stopped ? ` (${r.stopped})` : ''}: ${s.sent} pings, ${s.lost} unanswered`
      + (s.sent ? ` (${fmt1(100 * s.loss)} % loss)` : ''),
    '',
  ];
  const rows = [TABLE_HEAD, ...tableRows(r.board.role, s)];
  const widths = TABLE_HEAD.map((_, i) => Math.max(...rows.map((row) => row[i].length)));
  for (const row of rows) lines.push(row.map((c, i) => c.padEnd(widths[i])).join('  ').trimEnd());
  lines.push('', 'Each row is what that board received. Margin: dB above the SF\'s demodulation limit; 10th pct: 90 % of',
    'pings had at least this.', '', r.headline);
  for (const a of r.advice) lines.push(`- ${a}`);
  return `${lines.join('\n')}\n`;
}
