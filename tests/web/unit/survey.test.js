// The site survey (web/js/survey.js) against the test vectors tools/gatelink_client/survey.py is checked against too
// (tests/tools/fixtures/survey-vectors.json, tests/tools/test_survey.py), so the web console and `gatelink.py survey`
// judge a link alike.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as sv from '../../../web/js/survey.js';

const read = (p) => JSON.parse(readFileSync(new URL(`../../../${p}`, import.meta.url), 'utf8'));
const vec = read('tests/tools/fixtures/survey-vectors.json');
const fx = read('tests/web/fixtures/firmware.json');

// Equal, numbers to 1e-9 (both languages compute in IEEE doubles, but the vectors are JSON text).
function close(actual, expected, path = 'value') {
  if (typeof expected === 'number') {
    assert.equal(typeof actual, 'number', `${path}: ${actual}`);
    assert.ok(Math.abs(actual - expected) < 1e-9, `${path}: ${actual} != ${expected}`);
  } else if (Array.isArray(expected)) {
    assert.equal(actual.length, expected.length, path);
    expected.forEach((e, i) => close(actual[i], e, `${path}[${i}]`));
  } else if (expected !== null && typeof expected === 'object') {
    assert.deepEqual(Object.keys(actual).sort(), Object.keys(expected).sort(), path);
    for (const [k, v] of Object.entries(expected)) close(actual[k], v, `${path}.${k}`);
  } else {
    assert.equal(actual, expected, path);
  }
}

test('SNR floors per SF', () => close(sv.SNR_FLOOR, vec.floors, 'floors'));

test('one-decimal formatting rounds ties up, as in Python', () => {
  for (const { x, out } of vec.fmt1) assert.equal(sv.fmt1(x), out, String(x));
});

test('per-sample margin', () => {
  for (const m of vec.margins) close(sv.sampleMargin(m.sf, m.snr, m.rssi, m.noise), m.margin, JSON.stringify(m));
});

test('quantiles', () => {
  for (const q of vec.quantiles) close(sv.quantile(q.values, q.q), q.out, JSON.stringify(q));
});

for (const c of vec.cases) {
  test(`case: ${c.name}`, () => {
    const r = sv.analyze(c.settings, c.samples, c.role);
    close(r.summary, c.summary, 'summary');
    assert.equal(r.verdict, c.verdict);
    assert.equal(r.headline, c.headline);
    assert.deepEqual(r.advice, c.advice);
  });
}

test('the tx_power and sf limits are the firmware’s', () => {
  const meta = Object.fromEntries(fx.meta.map((m) => [m.name, m]));
  assert.equal(sv.TX_POWER_MIN, meta.tx_power.min);
  assert.equal(sv.TX_POWER_MAX, meta.tx_power.max);
  assert.equal(sv.SF_MAX, meta.sf.max);
  for (let sf = meta.sf.min; sf <= meta.sf.max; sf++) assert.notEqual(sv.snrFloor(sf), null, `SF${sf}`);
});

test('samples from pongs and status', () => {
  assert.deepEqual(sv.noiseFromStatus({ ...fx.status_common, ...fx.status_house, role: 'house' }), { noise: -118, peer_noise: -117 });
  // The gate doesn't know the house's noise floor; null before the first reading.
  assert.deepEqual(sv.noiseFromStatus({ role: 'gate', link: { noise: null }, remote: { noise: -117 } }), { noise: null, peer_noise: null });
  const pong = { event: 'pong', ping_id: 4, rtt_ms: 142, rssi: -61, snr: 9.25, peer_rssi: -60, peer_snr: 9, fei: -1450 };
  assert.deepEqual(sv.sampleFromPong(pong, 2, { noise: -118, peer_noise: null }), {
    t: 2, lost: false, rtt_ms: 142, rssi: -61, snr: 9.25, peer_rssi: -60, peer_snr: 9, fei: -1450, noise: -118, peer_noise: null,
  });
  assert.deepEqual(sv.sampleFromPong(null, 4), { t: 4, lost: true, noise: null, peer_noise: null });
});

test('the plain-text report has the settings, the table, the verdict and the advice', () => {
  const c = vec.cases[0];
  const r = sv.analyze(c.settings, c.samples, c.role);
  const text = sv.surveyText({ time: new Date(2026, 9, 10, 14, 3), board: { role: 'house', fw: '0.14.0' }, settings: c.settings,
    elapsed_s: 50.2, stopped: 'stopped', ...r });
  const lines = text.split('\n');
  assert.ok(lines.includes('Board: house, firmware 0.14.0'));
  assert.ok(lines.includes('Radio: SF9, 500 kHz, tx_power 17 dBm on this board'));
  assert.ok(lines.includes('Ran 0:50 (stopped): 25 pings, 0 unanswered (0.0 % loss)'));
  assert.ok(lines.some((l) => l.startsWith('at the house') && l.includes('67.5 / 67.5 dB')));
  assert.ok(lines.some((l) => l.startsWith('at the gate')));
  assert.ok(lines.includes(r.headline));
  assert.ok(lines.includes(`- ${r.advice[0]}`));
});
