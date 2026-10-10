// Formatting and chart helpers (web/js/util.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fmtDur, fmtNum, cmpVer, niceScale, wrapText, esc } from '../../../web/js/util.js';

test('durations', () => {
  assert.equal(fmtDur(-1), 'never');
  assert.equal(fmtDur(59999), '59s');
  assert.equal(fmtDur(65000), '1m 5s');
  assert.equal(fmtDur(3723000), '1h 2m');
  assert.equal(fmtDur(47 * 3600000), '47h 0m');
  assert.equal(fmtDur(50 * 3600000), '2d 2h');
});

test('numbers drop a trailing .0 and show missing values as a dash', () => {
  assert.equal(fmtNum(-61.25), '-61.3');
  assert.equal(fmtNum(9), '9');
  assert.equal(fmtNum(null), '—');
});

test('versions compare numerically', () => {
  assert.ok(cmpVer('0.13.10', '0.13.9') > 0);
  assert.ok(cmpVer('0.4.2', '0.5.0') < 0);
  assert.equal(cmpVer('1.0', '1.0.0'), 0);
});

test('chart scales give round ticks covering the range', () => {
  assert.deepEqual(niceScale(-118, -112), { lo: -118, hi: -112, ticks: [-118, -116, -114, -112] });
  assert.deepEqual(niceScale(-20, 12), { lo: -20, hi: 20, ticks: [-20, 0, 20] });
  const s = niceScale(3, 3);
  assert.ok(s.hi > s.lo);
});

test('word wrap and escaping', () => {
  assert.deepEqual(wrapText('MKR WAN 1310 + Relay Proto Shield', 16), ['MKR WAN 1310 +', 'Relay Proto', 'Shield']);
  assert.equal(esc('<a href="x">&</a>'), '&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;');
});
