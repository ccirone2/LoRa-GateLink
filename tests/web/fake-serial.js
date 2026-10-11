// Injected into the page before it loads (page.addInitScript in the browser tests): navigator.serial backed by fake
// GateLink boards that answer the console protocol (docs/console.md) from tests/web/fixtures/firmware.json. A test
// configures the boards through window.__fakeSerialConfig (set by an earlier init script) and drives them through
// window.__fake: push events, unplug and replug, read back the requests the page sent.
(() => {
  const cfg = window.__fakeSerialConfig;
  if (!cfg) return;
  if (cfg.noSerial) {
    delete Navigator.prototype.serial; // a browser without Web Serial
    return;
  }
  const enc = new TextEncoder();
  const dec = new TextDecoder();
  const clone = (o) => JSON.parse(JSON.stringify(o));
  const fx = cfg.fixture;
  const ROLE_NUM = { unset: 0, house: 1, gate: 2 };
  const ROLE_NAME = ['unset', 'house', 'gate'];
  const verAtLeast = (v, min) => {
    const a = v.split('.').map(Number), b = min.split('.').map(Number);
    for (let i = 0; i < 3; i++) if ((a[i] || 0) !== (b[i] || 0)) return (a[i] || 0) > (b[i] || 0);
    return true;
  };
  const hexBytes = (hex) => Uint8Array.from(hex.match(/../g), (b) => parseInt(b, 16));

  // The key id as the firmware computes it (config.cpp configKeyId): HMAC-SHA256(key, label), first 4 bytes.
  async function keyIdOf(hex) {
    const k = await crypto.subtle.importKey('raw', hexBytes(hex), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const mac = new Uint8Array(await crypto.subtle.sign('HMAC', k, enc.encode(fx.key.label)));
    return [...mac.slice(0, 4)].map((x) => x.toString(16).padStart(2, '0')).join('');
  }

  // config.cpp configKeyWeak: all bytes equal, counting up or down by one (mod 256), or 8 or fewer distinct values.
  function weakKey(hex) {
    const b = hexBytes(hex);
    const steps = new Set([...b].slice(1).map((x, i) => (x - b[i] + 256) % 256));
    return new Set(b).size <= 8 || (steps.size === 1 && (steps.has(1) || steps.has(255)));
  }

  // navigator.serial's connect/disconnect listeners; Chrome's events carry the port as target.
  const listeners = { connect: [], disconnect: [] };
  const fire = (type, port) => listeners[type].forEach((f) => f({ type, target: port }));

  // resetAt: the bucket the board reset before (firmware 0.14.0 keeps the history across resets), or -1.
  function histRows(n, resetAt = -1) {
    const rows = [];
    for (let i = 0; i < n; i++) {
      const row = {
        boot: i < resetAt ? 41 : 42,
        idx: i, tx: 120 + i, rx: 130 + i, retries: i % 7 === 3 ? 2 : 0, giveups: i === n - 4 ? 1 : 0, crc_err: i % 11 === 5 ? 1 : 0,
        mac_fail: 0, lbt_defers: 1, lbt_forced: 0, sessions: i === 0 ? 1 : 0, radio_faults: 0, down_s: i === n - 4 ? 90 : 0,
        rssi_min: -66, rssi_avg: -61.25, snr_min: 7.5, snr_avg: 9.25, noise_avg: -118, noise_max: -112, peer_n: 120,
        peer_rssi_min: -65, peer_rssi_avg: -60.5, peer_snr_min: 8, peer_snr_avg: 9, peer_noise_avg: -117, peer_noise_max: -111,
        peer_retries: 0, peer_giveups: 0, peer_crc_err: 0,
      };
      rows.push(fx.hist_fields.map((f) => row[f]));
    }
    return rows;
  }

  class Board {
    constructor(spec, index) {
      this.spec = spec;
      this.index = index;
      this.params = { ...clone(fx.params), role: ROLE_NUM[spec.role ?? 'house'], ...(spec.params || {}) };
      this.saved = clone(this.params);
      this.role = ROLE_NAME[this.params.role]; // latched at boot, as activeRole
      this.status = { ...clone(fx.status_common), role: this.role, ...clone(fx[`status_${this.role}`] || {}), ...(spec.status || {}) };
      this.uptimeMs = this.status.uptime_ms;
      this.keySet = spec.keySet ?? true;
      this.fw = spec.fw || fx.status_common.fw;
      this.key = this.keySet ? (spec.key ?? fx.key.default) : null; // what key.set wrote (the page can't read it)
      this.keyId = null;
      this.keyReady = this.setKey(this.key);
      this.hasKeyId = verAtLeast(this.fw, fx.key.since_fw); // older firmware reports no key_id
      this.log = spec.log || [];
      this.histN = spec.histBuckets ?? 30;
      this.histResetAt = spec.histResetAt ?? -1;
      this.requests = [];
      this.pings = 0;
      this.replyDelayMs = spec.replyDelayMs ?? 5;
      this.silent = false; // stops answering (a hung board)
    }

    async setKey(hex) {
      this.key = hex;
      this.keyId = hex ? await keyIdOf(hex) : null;
    }

    // key_set and, from firmware 0.13.8, key_id (null without a key).
    keyFields() {
      return { key_set: this.keySet, ...(this.hasKeyId ? { key_id: this.keySet ? this.keyId : null } : {}) };
    }

    // info's saved_role is the running config's role (cfg.role), which a reboot makes the active one. spec.roleText
    // replaces the role string info and status report (only to check the page treats it as text).
    info() {
      return { fw: this.fw, board: 'MKR WAN 1310', role: this.spec.roleText ?? this.role,
        saved_role: ROLE_NAME[this.params.role], ...this.keyFields(),
        cfg_store: this.spec.cfgStore || 'spi', flash_id: 'ef4015', boot_count: 7 };
    }

    // The status reply, from the board's state as appFillStatus builds it.
    statusNow() {
      this.uptimeMs += 2000;
      const s = { ...clone(this.status), fw: this.fw, role: this.spec.roleText ?? this.role, uptime_ms: this.uptimeMs,
        key_set: this.keySet,
        cfg_store: this.spec.cfgStore || 'spi', reboot_pending: this.params.role !== ROLE_NUM[this.role] };
      // D5 follows fault_out at once (health.cpp): null while it's off, else high unless a test gave a level. The
      // hold isn't modelled: a test that wants D5 low with problems listed sets both.
      if ('fault_out' in s) s.fault_out = !this.params.fault_out ? null : s.fault_out ?? (s.health ?? []).length === 0;
      return s;
    }

    // A reset: the saved config comes back and its role takes effect.
    restart() {
      this.params = clone(this.saved);
      this.role = ROLE_NAME[this.params.role];
      this.uptimeMs = 0;
    }

    emit(obj, delay = 20) {
      setTimeout(() => this.port?.push(obj), delay);
    }

    // The reply, or a promise of it (key ids take WebCrypto), or null for none.
    handle(req) {
      this.requests.push(req);
      if (this.silent) return null;
      const ok = (extra = {}) => ({ id: req.id, ok: true, ...extra });
      const err = (error, extra = {}) => ({ id: req.id, ok: false, error, ...extra });
      const meta = fx.meta;
      switch (req.cmd) {
        case 'info': return this.keyReady.then(() => ok(this.info()));
        case 'status': return ok({ status: this.statusNow() });
        case 'config.get': return this.keyReady.then(() => ok({ params: clone(this.params), meta: clone(meta), ...this.keyFields() }));
        case 'config.set': {
          const applied = [], errors = [];
          let reboot = false;
          for (const [k, v] of Object.entries(req.params || {})) {
            const m = meta.find((x) => x.name === k);
            if (m && Number.isInteger(v) && this.params[k] === v) continue;
            if (!m || !Number.isInteger(v) || v < m.min || v > m.max) {
              errors.push(k);
              continue;
            }
            this.params[k] = v;
            applied.push(k);
            reboot ||= m.reboot;
          }
          return { id: req.id, ok: errors.length === 0, applied, errors, reboot_required: reboot };
        }
        case 'config.save':
          this.saved = clone(this.params);
          return ok();
        case 'config.reset':
          this.params = clone(fx.params);
          this.saved = clone(fx.params);
          this.keySet = false;
          this.keyReady = this.setKey(null);
          return ok({ reboot_required: true });
        case 'key.set':
          if (typeof req.key !== 'string' || !/^[0-9a-f]{32}$/i.test(req.key)) return err('key must be 32 hex chars');
          if (this.hasKeyId && weakKey(req.key)) return err(fx.key.weak_error);
          this.keySet = true;
          this.keyReady = this.setKey(req.key.toLowerCase());
          return this.keyReady.then(() => ok());
        case 'relay.test':
          if (![1, 2].includes(req.k) || !Number.isInteger(req.ms ?? 500) || (req.ms ?? 500) < 50 || (req.ms ?? 500) > 5000) {
            return err('k must be 1|2, ms 50..5000, role set');
          }
          return ok();
        case 'radio.ping': {
          // spec.pongs: the answers to successive pings, cycled; each overrides these levels, null = no answer. With
          // `late` (levels), the previous ping's pong lands first, just before this ping reaches the board, as one
          // past the page's 8 s can. As in the firmware, a pong is reported only while its ping is the latest.
          const p = this.spec.pongs ? this.spec.pongs[this.pings % this.spec.pongs.length] : {};
          const pong = (id, levels) => ({ event: 'pong', ping_id: id, rtt_ms: 142, rssi: -61, snr: 9.25, peer_rssi: -60,
            peer_snr: 9, fei: -1450, ...levels });
          if (p?.late) this.port?.push(pong(this.pings, { rtt_ms: 8100, ...p.late }));
          const id = ++this.pings;
          if (p !== null) {
            const levels = { ...p };
            delete levels.late;
            setTimeout(() => { if (this.pings === id) this.port?.push(pong(id, levels)); }, 20);
          }
          return ok();
        }
        case 'remote.diag':
          if (this.role !== 'house') return err('house node only');
          this.emit({ event: 'remote_diag', fw: this.fw, uptime_s: 3600,
            counters: { tx: 10, rx: 12, mac_fail: 0, replay: 0, retries: 1, giveups: 0 }, params: { retries: 5, heartbeat_s: 30 } });
          return ok();
        case 'remote.set': {
          const m = meta.find((x) => x.name === req.name);
          if (!Number.isInteger(req.value)) return err('value must be an integer');
          if (this.role !== 'house' || !m || !m.remote || req.value < m.min || req.value > m.max) {
            return err('house node only; param must be remote-writable and in range');
          }
          this.emit({ event: 'remote_set', acked: true, ok: true, applied: true });
          return ok();
        }
        case 'log.get': return ok({ log: clone(this.log), now: this.uptimeMs });
        case 'hist.get': {
          const rows = histRows(this.histN, this.histResetAt);
          const current = this.histN - 1;
          const from = req.from ?? 0;
          const n = Math.min(req.n ?? 12, 12);
          return ok({ period_s: 3600, now_s: current * 3600 + 1800, oldest: 0, current, persist: true, fields: fx.hist_fields,
            rows: rows.slice(from, from + n) });
        }
        case 'hist.clear':
          this.histN = 1;
          this.histResetAt = -1;
          return ok();
        case 'reboot':
          setTimeout(() => {
            this.restart();
            this.port.replug(this.spec.rebootMs ?? 300);
          }, 30);
          return ok();
        case 'identify': return ok();
        case 'debug.replay': return ok({ sent: true });
        default: return err('unknown cmd');
      }
    }
  }

  class FakePort {
    constructor(board) {
      this.board = board;
      board.port = this;
      this.isOpen = false;
      this.present = true;
      this.granted = board.spec.granted ?? true;
      this.dtr = false;
      this.opens = []; // baud rate of every open (1200 = the touch that resets into the bootloader)
    }

    getInfo() { return { usbVendorId: 0x2341, usbProductId: this.board.spec.bootloader ? 0x0059 : 0x8059 }; }

    async open(options) {
      if (!this.present) throw new DOMException('The device has been lost.', 'NetworkError');
      if (this.isOpen) throw new DOMException('The port is already open.', 'InvalidStateError');
      this.opens.push(options?.baudRate);
      this.isOpen = true;
      this.dtr = false;
      this.inbuf = '';
      this.readable = new ReadableStream({ start: (c) => { this.ctrl = c; } });
      this.writable = new WritableStream({ write: (chunk) => this.onBytes(chunk) });
    }

    // As in Chrome: a port whose streams are still locked (a reader or a pipe holding them) can't close.
    async close() {
      if (!this.isOpen) throw new DOMException('The port is already closed.', 'InvalidStateError');
      if (this.readable?.locked || this.writable?.locked) throw new TypeError('Cannot cancel a locked stream');
      this.isOpen = false;
      try { this.ctrl.close(); } catch { /* already errored or cancelled */ }
    }

    async setSignals(s) {
      if (!this.isOpen) throw new DOMException('The port is closed.', 'InvalidStateError');
      if ('dataTerminalReady' in s) this.dtr = s.dataTerminalReady;
    }

    async forget() { this.granted = false; }

    onBytes(chunk) {
      this.inbuf += typeof chunk === 'string' ? chunk : dec.decode(chunk);
      let i;
      while ((i = this.inbuf.indexOf('\n')) >= 0) {
        const line = this.inbuf.slice(0, i);
        this.inbuf = this.inbuf.slice(i + 1);
        let req;
        try { req = JSON.parse(line); } catch {
          this.push({ ok: false, error: 'bad json' });
          continue;
        }
        Promise.resolve(this.board.handle(req)).then((res) => {
          if (res) setTimeout(() => this.push(res), this.board.replyDelayMs);
        });
      }
    }

    push(obj) { this.pushLine(JSON.stringify(obj)); }

    pushLine(text) {
      if (this.isOpen && this.dtr && this.ctrl) {
        try { this.ctrl.enqueue(enc.encode(`${text}\n`)); } catch { /* the page cancelled the stream */ }
      }
    }

    unplug() {
      this.present = false;
      if (this.isOpen) {
        this.isOpen = false;
        try { this.ctrl.error(new DOMException('The device has been lost.', 'NetworkError')); } catch { /* closed */ }
      }
      fire('disconnect', this);
    }

    plug() {
      this.present = true;
      fire('connect', this);
    }

    replug(ms) {
      this.unplug();
      setTimeout(() => this.plug(), ms);
    }
  }

  const boards = (cfg.boards || [{ role: 'house' }]).map((spec, i) => new Board(spec, i));
  const ports = boards.map((b) => new FakePort(b));
  const serial = {
    async getPorts() { return ports.filter((p) => p.granted && p.present); },
    async requestPort() {
      const p = ports.find((x) => !x.granted && x.present) || (cfg.chooserPicks !== undefined ? ports[cfg.chooserPicks] : null);
      if (!p) throw new DOMException('No port selected by the user.', 'NotFoundError');
      p.granted = true;
      return p;
    },
    addEventListener(type, f) { listeners[type]?.push(f); },
    removeEventListener(type, f) { listeners[type] = (listeners[type] || []).filter((x) => x !== f); },
  };
  Object.defineProperty(Navigator.prototype, 'serial', { get: () => serial, configurable: true });
  window.__fake = { boards, ports };
})();
