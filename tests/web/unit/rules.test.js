// Project rules the page must keep (CLAUDE.md, "Web console").
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';

const web = new URL('../../../web/', import.meta.url);
const files = ['index.html', 'style.css', 'app.js', ...readdirSync(new URL('js/', web)).map((f) => `js/${f}`)];

test('naming rule: the page calls the house-side device "the controller", never by brand', () => {
  for (const f of files) {
    const text = readFileSync(new URL(f, web), 'utf8');
    assert.doesNotMatch(text, /shelly|alarm\.com/i, `${f} names the install's controller hardware`);
  }
});

test('every module is loaded as an ES module from the page', () => {
  const html = readFileSync(new URL('index.html', web), 'utf8');
  assert.match(html, /<script type="module" src="app\.js"><\/script>/);
  const app = readFileSync(new URL('app.js', web), 'utf8');
  const imported = new Set([...app.matchAll(/from '\.\/js\/([a-z]+\.js)'/g)].map((m) => m[1]));
  for (const f of readdirSync(new URL('js/', web))) {
    // Modules only other modules import are still reached: check each one is imported somewhere.
    const used = imported.has(f) || files.some((g) => g.startsWith('js/') && readFileSync(new URL(g, web), 'utf8').includes(`'./${f}'`));
    assert.ok(used, `web/js/${f} isn't imported by anything`);
  }
});
