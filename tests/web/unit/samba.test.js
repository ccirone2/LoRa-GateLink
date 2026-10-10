// Firmware image checks and the bootloader's CRC (web/js/samba.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { crc16, parseFirmware, APP_MAX, FW_MARKER } from '../../../web/js/samba.js';

function image({ marker = `${FW_MARKER}0.14.0`, size = 4096, sp = 0x20008000, reset = 0x2101 } = {}) {
  const b = new Uint8Array(size).fill(0xff);
  const dv = new DataView(b.buffer);
  dv.setUint32(0, sp, true);
  dv.setUint32(4, reset, true);
  if (marker && size >= 1100) b.set(new TextEncoder().encode(marker), 1024);
  return b.buffer;
}

test('crc16 is CRC-16/XMODEM, as the bootloader computes it', () => {
  assert.equal(crc16(new TextEncoder().encode('123456789')), 0x31c3); // the algorithm's check value
  assert.equal(crc16([]), 0);
});

test('a GateLink image is accepted, padded to whole pages, with its version', () => {
  const fw = parseFirmware(image({ size: 4100 }), 'GateLink.bin');
  assert.equal(fw.version, '0.14.0');
  assert.equal(fw.bytes.length, 4160); // 65 pages of 64 bytes
  assert.equal(fw.bytes[4100], 0xff);
});

test('an image without the marker has no version', () => {
  assert.equal(parseFirmware(image({ marker: '' }), 'blink.bin').version, null);
});

test('files that aren’t application images for this board are refused', () => {
  assert.throws(() => parseFirmware(image({ size: 512 }), 'tiny.bin'), /not a firmware image/);
  assert.throws(() => parseFirmware(image({ size: APP_MAX + 64 }), 'huge.bin'), /not a firmware image/);
  assert.throws(() => parseFirmware(image({ sp: 0x10000000 }), 'x.bin'), /not a firmware image/); // stack outside RAM
  assert.throws(() => parseFirmware(image({ reset: 0x2100 }), 'x.bin'), /not a firmware image/); // not Thumb
  assert.throws(() => parseFirmware(image({ reset: 0x1001 }), 'x.bin'), /not a firmware image/); // inside the bootloader
});
