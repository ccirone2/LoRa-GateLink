# Web console tests

The web console (`web/`) is plain ES modules with no build step. Its development tools live in the repo root's
`package.json` (Node.js 22):

```sh
npm ci              # once
npm test            # all of it: lint, unit tests, browser tests
npm run lint        # ESLint (eslint.config.js): web/ and these tests
npm run test:unit   # node --test tests/web/unit/*.test.js
npm run test:browser  # Playwright, Chromium (first time: npx playwright install chromium)
```

CI runs the same (`ci.yml`, job `web`) and keeps the Playwright report as an artifact when a browser test fails.

## Unit tests (`unit/`)

`node --test` on the modules that don't touch the page: `samba.js` (firmware image checks, the bootloader's CRC),
`logdecode.js` (log events as text), `keys.js` (key ids, the weak-key rule, encrypted key backups) and `util.js`.
Some also read the firmware sources, so the page can't drift from them: every log event in `log.cpp` must have a
decoder, the gate states and causes must match `roles.h`, and the key id label must match `config.cpp`. The key
backup tests also open a backup written by the Python tools (`tests/tools/fixtures/key-backup-v1-python.glkey`), and
`tests/tools` opens one written by `keys.js` (`key-backup-v1.glkey`), so the two implementations stay one format.

## Browser tests (`browser/`)

The real page, served by `serve.js` (port 47123), in Chromium, with `navigator.serial` replaced by fake boards
(`fake-serial.js`, injected before the page loads). The fakes answer the console protocol (`docs/console.md`) from
`fixtures/firmware.json`, which has the firmware's settings table, defaults and status fields, and the key id
label and test vectors (a fake board computes its `key_id` with WebCrypto, as the firmware does, and refuses weak
keys from firmware 0.13.8; give a board `key` to hold another key than the default). A test:

```js
import { openConsole, connect, requests, pushEvent } from './helpers.js';

test('…', async ({ page }) => {
  await openConsole(page, [{ role: 'house', status: { link_up: false } }]); // boards: role, status, params, log, fw…
  await connect(page);                                  // through the board picker
  await pushEvent(page, { event: 'log', t: 1000, ev: 'link_down', a: 0, b: 0 }); // as the firmware would
  expect((await requests(page)).map((r) => r.cmd)).toContain('status');          // what the page sent
});
```

`window.__fake` in the page gives the boards (`handle` can be wrapped to make one hang or fail) and their ports
(`unplug`, `plug`, `replug`, `push`, `pushLine`). Dialogs are accepted unless `openConsole(…, { dismissDialogs: true })`;
their texts are in `page.dialogs`.

Flashing itself needs the real bootloader and stays a bench check (`docs/bench-testing.md`); the image checks before it
are covered here.

When the console protocol changes, change `fake-serial.js` and `fixtures/firmware.json` with it
([docs/architecture.md](../../docs/architecture.md), "Console").
