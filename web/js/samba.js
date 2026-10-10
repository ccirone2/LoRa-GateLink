// Firmware images and the board's bootloader. No page access here: unit-tested under Node (tests/web/unit).
//
// The board's Arduino bootloader (SAM-BA with the Arduino X/Y/Z extensions, what bossac talks to) is
// driven over Web Serial. A 1200-baud open/close makes the running firmware reset into it; it then
// enumerates as a different USB device (PID 0x0059), which needs its own one-time port grant. Config and
// key live in the SPI flash chip (0.5.0 on), which the bootloader never touches. A failed or interrupted
// update leaves the application erased, so the bootloader stays in charge and flashing again finishes it.
import { sleep, hex8 } from './util.js';

export const USB_VID = 0x2341;
export const BOOT_PID = 0x0059;
export const APP_START = 0x2000; // after the 8 KB bootloader
export const APP_MAX = 0x40000 - APP_START;
const RAM_BUF = 0x20005000; // bootloader's free RAM, staged here before each flash write
export const CHUNK = 4096; // bytes staged and written per S/Y round
export const FW_MARKER = 'GATELINK_FW='; // config.h FW_MARKER_PREFIX; firmware 0.5.1 on

export const isBootPort = (p) => {
  const i = p.getInfo();
  return i.usbVendorId === USB_VID && i.usbProductId === BOOT_PID;
};

// CRC-16/XMODEM, as the bootloader's Z command computes it.
export function crc16(bytes) {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

// Checks a .bin is an application image for this board and finds its GateLink version marker.
// Returns { bytes (padded to whole 64-byte flash pages), version or null }.
export function parseFirmware(buf, name) {
  const raw = new Uint8Array(buf);
  if (raw.length < 1024 || raw.length > APP_MAX) throw new Error(`${name}: ${raw.length} bytes is not a firmware image for this board.`);
  const dv = new DataView(buf);
  const sp = dv.getUint32(0, true);
  const reset = dv.getUint32(4, true);
  if (sp <= 0x20000000 || sp > 0x20008000 || !(reset & 1) || reset < APP_START || reset >= 0x40000) {
    throw new Error(`${name} is not a firmware image for this board (a .bin built for the MKR WAN 1310 is needed, not .hex or .elf).`);
  }
  let version = null;
  const text = new TextDecoder('latin1').decode(raw);
  const at = text.indexOf(FW_MARKER);
  if (at >= 0) version = /^[0-9]+\.[0-9]+\.[0-9]+/.exec(text.slice(at + FW_MARKER.length, at + FW_MARKER.length + 16))?.[0] || null;
  const bytes = new Uint8Array(Math.ceil(raw.length / 64) * 64).fill(0xff);
  bytes.set(raw);
  return { bytes, version, name };
}

export class SamBa {
  constructor(p) {
    this.port = p;
    this.buf = new Uint8Array(0);
    this.wake = null;
    this.ended = false;
  }

  async open() {
    await this.port.open({ baudRate: 115200 }); // USB CDC: the rate is ignored
    this.writer = this.port.writable.getWriter();
    this.reader = this.port.readable.getReader();
    this.loop = (async () => {
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          const b = new Uint8Array(this.buf.length + value.length);
          b.set(this.buf);
          b.set(value, this.buf.length);
          this.buf = b;
          this.wake?.();
        }
      } catch {}
      this.ended = true;
      this.wake?.();
    })();
  }

  async close() {
    try { await this.reader.cancel(); } catch {}
    await this.loop;
    try { this.reader.releaseLock(); } catch {}
    try { this.writer.releaseLock(); } catch {}
    await this.port.close().catch(() => {});
  }

  async write(data) {
    await this.writer.write(typeof data === 'string' ? new TextEncoder().encode(data) : data);
  }

  // Replies end in "\n\r".
  async reply(what, ms) {
    const until = Date.now() + ms;
    for (;;) {
      for (let i = 1; i < this.buf.length; i++) {
        if (this.buf[i - 1] === 10 && this.buf[i] === 13) {
          const line = new TextDecoder('latin1').decode(this.buf.subarray(0, i - 1));
          this.buf = this.buf.slice(i + 1);
          return line;
        }
      }
      if (this.ended) throw new Error(`${what}: the board went away`);
      const left = until - Date.now();
      if (left <= 0) {
        const got = this.buf.length ? ` (got ${JSON.stringify(new TextDecoder('latin1').decode(this.buf.subarray(0, 32)))})` : '';
        throw new Error(`${what}: no answer from the bootloader${got}.`);
      }
      await new Promise((r) => { this.wake = r; setTimeout(r, left); });
      this.wake = null;
    }
  }

  async cmd(text, what, ms = 2000) {
    await this.write(text);
    return this.reply(what, ms);
  }

  // Binary mode, then the version line, which must advertise the Arduino extensions used here.
  async hello() {
    await this.write('N#');
    await this.reply('N', 500).catch(() => {}); // only answered if the mode was already binary
    this.buf = new Uint8Array(0);
    const v = (await this.cmd('V#', 'version')).trim();
    const ext = /\[Arduino:([A-Z]+)\]/.exec(v)?.[1] || '';
    if (!['X', 'Y', 'Z'].every((c) => ext.includes(c))) throw new Error(`Unsupported bootloader: ${v}`);
    return v;
  }

  async erase(addr) {
    const r = await this.cmd(`X${hex8(addr)}#`, 'erase', 20000);
    if (r !== 'X') throw new Error(`erase: unexpected reply "${r}"`);
  }

  // Stage bytes in RAM, then copy them to flash. The data must reach the board as separate USB transfers
  // from its S command and from the next command (the bootloader mishandles them sharing packets; bossac
  // flushes in between). Web Serial's write() resolves once the bytes are queued, not sent, so back-to-back
  // writes can merge: wait for each to drain.
  async program(dst, data) {
    const at = `write at 0x${hex8(dst)}`;
    await this.write(`S${hex8(RAM_BUF)},${hex8(data.length)}#`);
    await sleep(20);
    await this.write(data.slice()); // own buffer: the caller's view is reused
    await sleep(30);
    let r = await this.cmd(`Y${hex8(RAM_BUF)},0#`, at);
    if (r === 'Y') r = await this.cmd(`Y${hex8(dst)},${hex8(data.length)}#`, at, 5000);
    if (r !== 'Y') throw new Error(`${at}: unexpected reply "${r}"`);
  }

  async crc(addr, len) {
    const r = await this.cmd(`Z${hex8(addr)},${hex8(len)}#`, 'verify', 10000);
    const m = /^Z([0-9A-Fa-f]{8})#$/.exec(r);
    if (!m) throw new Error(`verify: unexpected reply "${r}"`);
    return parseInt(m[1], 16);
  }

  // SYSRESETREQ: the bootloader sees a valid application and starts it. No reply.
  async reset() {
    await this.write('WE000ED0C,05FA0004#');
  }
}
