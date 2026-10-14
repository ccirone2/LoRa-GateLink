// The Install tab's setup checklist (web/js/checklist.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setupSteps } from '../../../web/js/checklist.js';

const matched = { there: { params: {} }, diffs: [], fwMinor: false };
const done = (ctx) => setupSteps(ctx).map((s) => s.done);

test('a finished install ticks every step', () => {
  assert.deepEqual(done({ role: 'house', keySet: true, peer: matched, linkUp: true, survey: 'fair' }), [true, true, true, true, true]);
});

test('each step waits for its own condition', () => {
  assert.deepEqual(done({ role: 'unset' }), [false, false, false, false, false]);
  assert.equal(done({ role: 'gate', rebootPending: true })[0], false); // saved, not running yet
  // The other board seen only through the house's link (its firmware, no settings) isn't a match yet.
  assert.equal(done({ role: 'house', peer: { there: { fw: '0.15.2' }, diffs: [], fwMinor: false } })[2], false);
  assert.equal(done({ role: 'house', keySet: true, peer: { ...matched, diffs: [{ name: 'sf' }] } })[2], false);
  assert.equal(done({ role: 'house', keySet: false, peer: matched })[2], false); // no key on either: no pair yet
  assert.equal(done({ role: 'house', keySet: true, peer: matched })[2], true);
  assert.equal(done({ role: 'house', keySet: true, peer: { ...matched, fwMinor: true } })[2], false);
  assert.equal(done({ role: 'house', survey: 'marginal' })[4], false);
  assert.equal(done({ role: 'house', survey: 'good' })[4], true);
});

test('steps name the other board and where each is done', () => {
  const steps = setupSteps({ role: 'gate' });
  assert.match(steps[2].text, /^The house board/);
  assert.deepEqual(steps.map((s) => s.where), ['Config', 'Board', 'Status → Other board', 'Status → Link', 'Link → Site survey']);
});
