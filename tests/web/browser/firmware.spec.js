// The firmware card's image checks, all before anything touches the board (flashing itself needs the bootloader: a
// bench check).
import { test, expect, openConsole, connect } from './helpers.js';

// A minimal application image: vector table (stack pointer in RAM, reset handler in flash above the bootloader).
function image({ marker = 'GATELINK_FW=0.14.0', size = 4096, sp = 0x20008000, reset = 0x2101 } = {}) {
  const b = Buffer.alloc(size, 0xff);
  b.writeUInt32LE(sp, 0);
  b.writeUInt32LE(reset, 4);
  if (marker) b.write(marker, 1024, 'latin1');
  return b;
}

const touched = (page) => page.evaluate(() => window.__fake.ports[0].opens.includes(1200)); // the bootloader reset

test('a file that isn’t an image for this board is refused', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#fwFile').setInputFiles({ name: 'sketch.hex', mimeType: 'application/octet-stream',
    buffer: Buffer.from(':100000000C9434000C9446000C9446000C9446006A\n'.repeat(80)) });
  await expect(page.locator('#toast')).toContainText('not a firmware image for this board');
  expect(await touched(page)).toBe(false);
});

test('an image without the GateLink marker needs a confirmation', async ({ page }) => {
  await openConsole(page, [{ role: 'house', cfgStore: 'internal' }], { dismissDialogs: true });
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#fwFile').setInputFiles({ name: 'blink.bin', mimeType: 'application/octet-stream', buffer: image({ marker: '' }) });
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('has no GateLink version marker');
  expect(page.dialogs[0]).not.toContain('older than 0.5.0'); // its config is in program flash anyway
  expect(await touched(page)).toBe(false);
});

test('without a marker, a board keeping its config on the flash chip is warned that pre-0.5.0 loses it', async ({ page }) => {
  await openConsole(page, [{ role: 'house' }], { dismissDialogs: true });
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#fwFile').setInputFiles({ name: 'GateLink-0.4.2.bin', mimeType: 'application/octet-stream',
    buffer: image({ marker: '' }) });
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('older than 0.5.0, the board comes back without its settings and key');
});

test('flashing an older image says so before anything happens', async ({ page }) => {
  await openConsole(page, [{ role: 'house', fw: '0.13.6' }], { dismissDialogs: true });
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#fwFile').setInputFiles({ name: 'old.bin', mimeType: 'application/octet-stream',
    buffer: image({ marker: 'GATELINK_FW=0.12.1' }) });
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('older than the firmware it runs now');
  expect(await touched(page)).toBe(false);
});

// Install latest: the page checks the bundled image (firmware/latest.json, written by the Pages deploy) before
// asking to flash it.
async function serveLatest(page, latest, bin) {
  await page.route('**/firmware/latest.json', (r) => r.fulfill({ json: latest }));
  await page.route(`**/firmware/${latest.file}`, (r) => r.fulfill({ body: bin, contentType: 'application/octet-stream' }));
}
const sha256 = async (b) => [...new Uint8Array(await crypto.subtle.digest('SHA-256', b))]
  .map((x) => x.toString(16).padStart(2, '0')).join('');

const MISMATCHES = [
  ['size', (l) => ({ ...l, size: l.size + 64 }), 'the download is damaged'],
  ['checksum', (l) => ({ ...l, sha256: '0'.repeat(64) }), 'checksum mismatch'],
  ['version', (l) => ({ ...l, version: '0.14.1' }), 'carries version 0.14.0, not 0.14.1'],
];
for (const [what, tweak, message] of MISMATCHES) {
  test(`Install latest refuses an image whose ${what} doesn't match latest.json`, async ({ page }) => {
    const bin = image({ marker: 'GATELINK_FW=0.14.0' });
    const latest = { version: '0.14.0', file: 'GateLink-v0.14.0.bin', sha256: await sha256(bin), size: bin.length };
    await serveLatest(page, tweak(latest), bin);
    await openConsole(page);
    await connect(page);
    await page.locator('#tabbtn-tools').click();
    await page.locator('#btnFwLatest').click();
    await expect(page.locator('#toast')).toContainText(message);
    expect(page.dialogs).toEqual([]); // refused before asking to flash
    expect(await touched(page)).toBe(false);
  });
}

test('Install latest offers the bundled release, checks it, then asks', async ({ page }) => {
  const bin = image({ marker: 'GATELINK_FW=0.14.0' });
  await serveLatest(page, { version: '0.14.0', file: 'GateLink-v0.14.0.bin', sha256: await sha256(bin), size: bin.length }, bin);
  await openConsole(page, [{ role: 'house' }], { dismissDialogs: true });
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await expect(page.locator('#btnFwLatest')).toHaveText('Install 0.14.0');
  await expect(page.locator('#fwLatest')).toHaveText('0.14.0 · update available');
  await page.locator('#btnFwLatest').click();
  await expect.poll(() => page.dialogs.length).toBe(1); // checks passed: asked to flash, and declined
  expect(page.dialogs[0]).toContain('Flash firmware 0.14.0 to this house board (running 0.13.6)?');
  expect(await touched(page)).toBe(false);
});
