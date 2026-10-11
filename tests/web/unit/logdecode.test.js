// Log events as text (web/js/logdecode.js), against the firmware's own event list (log.cpp, roles.h).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { decodeLog, rawLog, resetCause, KNOWN_EVENTS, STATES, CAUSES, PROBLEMS } from '../../../web/js/logdecode.js';

const fw = (f) => readFileSync(new URL(`../../../firmware/GateLink/${f}`, import.meta.url), 'utf8');

test('every log event the firmware can emit has a decoder', () => {
  const names = [...fw('log.cpp').match(/NAMES\[\]\s*=\s*\{([\s\S]*?)\};/)[1].matchAll(/"([a-z_0-9]+)"/g)].map((m) => m[1]);
  assert.ok(names.length > 20);
  assert.deepEqual(names.filter((n) => !KNOWN_EVENTS.includes(n)), []);
  assert.deepEqual(KNOWN_EVENTS.filter((n) => !names.includes(n)), []);
});

test('gate states and causes follow roles.h', () => {
  const roles = fw('roles.h');
  const gs = roles.match(/enum GateState[^{]*\{([^}]*)\}/)[1].split(',').map((s) => s.trim()).filter((s) => s && s !== 'GS_COUNT');
  assert.deepEqual(gs.map((s) => s.replace(/\s*=.*/, '').slice(3).toLowerCase()), STATES);
  const causes = roles.match(/enum Cause[^{]*\{([^}]*)\}/)[1].split(',').map((s) => s.trim().replace(/\s*=.*/, '')).filter(Boolean);
  assert.deepEqual(causes.map((s) => s.slice(6).toLowerCase()), CAUSES);
});

test('decoded lines', () => {
  assert.equal(decodeLog({ ev: 'gate_state', a: 3, b: 1 }), 'gate between (lora)');
  assert.equal(decodeLog({ ev: 'gate_state', a: 5, b: 0 }), 'gate no power');
  assert.equal(decodeLog({ ev: 'cmd_sent', a: 1, b: 42 }), 'OPEN command sent (#42)');
  assert.equal(decodeLog({ ev: 'pulse', a: 2, b: 500 }), 'K2 pulsed for 500 ms');
  assert.equal(decodeLog({ ev: 'ctrl', a: 0, b: 1 }), 'controller switched off (ignored: controller unpowered)');
  assert.equal(decodeLog({ ev: 'supply', a: -1, b: -1 }), 'charger not answering: board supply unknown');
  assert.equal(decodeLog({ ev: 'cfg', a: 1, b: 2 }), 'config loaded from the flash chip; 2 saved settings dropped');
  assert.equal(decodeLog({ ev: 'session', a: -1, b: 0 }), 'peer session ffffffff verified');
  assert.equal(decodeLog({ ev: 'cfg_remote', a: 16, b: 700 }, [{ id: 16, name: 'pulse_ms' }]), 'remote write: pulse_ms = 700');
  assert.equal(decodeLog({ ev: 'cfg_remote', a: 99, b: 1 }), 'remote write: param 99 = 1');
  assert.equal(decodeLog({ ev: 'something_new', a: 1, b: 2 }), 'something_new a=1 b=2');
  assert.equal(rawLog({ ev: 'pulse', a: 1, b: 500 }), 'pulse a=1 b=500');
});

test('health problems follow health.cpp, in bit order', () => {
  const names = [...fw('health.cpp').match(/healthName[\s\S]*?\[\]\s*=\s*\{([^}]*)\}/)[1].matchAll(/"([a-z_]+)"/g)].map((m) => m[1]);
  assert.deepEqual(Object.keys(PROBLEMS), names);
  const bits = [...fw('health.h').matchAll(/PROB_(\w+) = (0x[0-9A-Fa-f]+)/g)].map(([, n, v]) => [n.toLowerCase(), Number(v)]);
  assert.deepEqual(bits, names.map((n, i) => [n, 1 << i]));
  assert.equal(decodeLog({ ev: 'health', a: 1, b: 0 }), 'fault output high: healthy');
  assert.equal(decodeLog({ ev: 'health', a: 0, b: 0x0c }), 'fault output low: needs attention (link down, no AC power at the gate)');
  assert.equal(decodeLog({ ev: 'health', a: 0, b: 1 }), 'fault output low: needs attention (starting up)');
  assert.equal(decodeLog({ ev: 'health', a: -1, b: 0 }), 'fault output off (D5 an input again)');
});

test('reset causes as the firmware names them', () => {
  assert.equal(resetCause(0x20), 'watchdog');
  assert.equal(resetCause(0x22), 'watchdog'); // watchdog wins
  assert.equal(resetCause(0x04), 'brownout');
  assert.equal(resetCause(0x01), 'power-on');
  assert.equal(resetCause(0x10), 'reset pin');
  assert.equal(resetCause(0x40), 'software');
  assert.equal(resetCause(0), 'unknown');
});
