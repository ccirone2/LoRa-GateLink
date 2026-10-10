// The browser tests' fake board serves tests/web/fixtures/firmware.json: it must say what the firmware says. And
// the Config tab's groups must list every setting the firmware has.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { GROUPS } from '../../../web/js/settings.js';

const root = new URL('../../../', import.meta.url);
const read = (p) => readFileSync(new URL(p, root), 'utf8');
const fx = JSON.parse(read('tests/web/fixtures/firmware.json'));
const config = read('firmware/GateLink/config.cpp');

// PARAMS rows: { id, "name", &Config::field, min, max, flags }
const params = [...config.match(/PARAMS\[\]\s*=\s*\{([\s\S]*?)\n\};/)[1].matchAll(
  /\{\s*(\d+),\s*"([a-z_0-9]+)",\s*&Config::\w+,\s*(-?\d+),\s*(-?\d+),\s*([A-Z_|0 ]+)\s*\}/g)]
  .map(([, id, name, min, max, flags]) => ({ id: +id, name, min: +min, max: +max, flags }));

test('fixture meta = config.cpp PARAMS (ids, names, ranges, flags, order)', () => {
  assert.ok(params.length > 25);
  assert.deepEqual(fx.meta.map((m) => ({ id: m.id, name: m.name, min: m.min, max: m.max })),
    params.map(({ id, name, min, max }) => ({ id, name, min, max })));
  for (const p of params) {
    const m = fx.meta.find((x) => x.name === p.name);
    assert.equal(m.radio, p.flags.includes('P_RADIO'), `${p.name} radio`);
    assert.equal(m.remote, p.flags.includes('P_REMOTE'), `${p.name} remote`);
    assert.equal(m.reboot, p.flags.includes('P_REBOOT'), `${p.name} reboot`);
  }
});

test('fixture params = configDefaults()', () => {
  const body = config.match(/void configDefaults\(Config &c\) \{([\s\S]*?)\n\}/)[1];
  const defaults = {};
  for (const [, k, v] of body.matchAll(/c\.(\w+) = ([^;]+);/g)) defaults[k] = v.trim();
  const value = (v) => (v === 'ROLE_UNSET' ? 0 : Number(v));
  for (const p of params) {
    assert.equal(fx.params[p.name], value(defaults[p.name] ?? '0'), `default of ${p.name}`);
  }
  assert.deepEqual(Object.keys(fx.params).sort(), params.map((p) => p.name).sort());
});

test('fixture history fields = history.cpp FIELDS', () => {
  const src = read('firmware/GateLink/history.cpp');
  const fields = [...src.match(/FIELDS\[\]\s*=\s*\{([\s\S]*?)\};/)[1].matchAll(/"([a-z_0-9]+)"/g)].map((m) => m[1]);
  assert.deepEqual(fx.hist_fields, fields);
});

test('fixture status fields = what appFillStatus, houseStatus and gateStatus write', () => {
  const fw = (f) => (existsSync(new URL(`firmware/GateLink/${f}`, root)) ? read(`firmware/GateLink/${f}`) : '');
  const all = ['app.cpp', 'GateLink.ino', 'role_house.cpp', 'role_gate.cpp'].map(fw).join('\n');
  const body = (fn) => all.match(new RegExp(`void ${fn}\\(JsonObject o\\) \\{([\\s\\S]*?)\\n\\}`))[1];
  // Keys per object: o = the status itself, io, l = link, g = remote.
  const keys = (src, v) => new Set([...src.matchAll(new RegExp(`\\b${v}\\["([a-z_0-9]+)"\\]`, 'g'))].map((m) => m[1]));
  const objects = ['io', 'link', 'remote'];
  const top = (s) => Object.keys(s).filter((k) => !objects.includes(k)).sort();
  const common = body('appFillStatus');
  for (const role of ['house', 'gate']) {
    const own = body(`${role}Status`);
    const want = [...new Set([...keys(common, 'o'), ...keys(own, 'o')])].filter((k) => !objects.includes(k)).sort();
    // The fixture leaves out role: the fake board fills it in.
    const status = { ...fx.status_common, ...fx[`status_${role}`], role };
    assert.deepEqual(top(status), want, `${role} top-level fields`);
    assert.deepEqual(Object.keys(status.io).sort(), [...keys(common, 'io')].sort(), `${role} io`);
    assert.deepEqual(Object.keys(status.link).sort(), [...keys(common, 'l')].sort(), `${role} link`);
    if (role === 'house') assert.deepEqual(Object.keys(status.remote).sort(), [...keys(own, 'g')].sort(), 'house remote');
  }
});

test('the Config tab groups list every setting, once', () => {
  const grouped = GROUPS.flatMap(([, names]) => names);
  assert.equal(new Set(grouped).size, grouped.length, 'a setting in two groups');
  assert.deepEqual([...grouped].sort(), params.map((p) => p.name).sort());
});
