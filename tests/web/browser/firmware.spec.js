// The firmware card's image checks (the flashing itself needs the bootloader: covered on the bench).
import { test, expect } from '@playwright/test';
import { openConsole, connect } from './helpers.js';

// A minimal application image: vector table (stack pointer in RAM, reset handler in flash above the bootloader).
function image({ marker = 'GATELINK_FW=0.14.0', size = 4096, sp = 0x20008000, reset = 0x2101 } = {}) {
  const b = Buffer.alloc(size, 0xff);
  b.writeUInt32LE(sp, 0);
  b.writeUInt32LE(reset, 4);
  if (marker) b.write(marker, 1024, 'latin1');
  return b;
}

test('a file that isn’t an image for this board is refused', async ({ page }) => {
  await openConsole(page);
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#fwFile').setInputFiles({ name: 'sketch.hex', mimeType: 'application/octet-stream',
    buffer: Buffer.from(':100000000C9434000C9446000C9446000C9446006A\n'.repeat(80)) });
  await expect(page.locator('#toast')).toContainText('not a firmware image for this board');
});

test('an image without the GateLink marker needs a confirmation', async ({ page }) => {
  await openConsole(page, [{ role: 'house' }], { dismissDialogs: true });
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#fwFile').setInputFiles({ name: 'blink.bin', mimeType: 'application/octet-stream', buffer: image({ marker: '' }) });
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('has no GateLink version marker');
});

test('flashing an older image says so before anything happens', async ({ page }) => {
  await openConsole(page, [{ role: 'house', fw: '0.13.6' }], { dismissDialogs: true });
  await connect(page);
  await page.locator('#tabbtn-tools').click();
  await page.locator('#fwFile').setInputFiles({ name: 'old.bin', mimeType: 'application/octet-stream',
    buffer: image({ marker: 'GATELINK_FW=0.4.2' }) });
  await expect.poll(() => page.dialogs.length).toBe(1);
  expect(page.dialogs[0]).toContain('older than the firmware it runs now');
  expect(page.dialogs[0]).toContain('before 0.5.0 keeps config and key in program flash');
});
