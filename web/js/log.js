// The Log tab's controls: the board's log events, decoded (logdecode.js), its stored log, raw serial and the
// download. The view itself is ui.js's, as every module writes to it.
import { $, fmtDur, download } from './util.js';
import { S } from './state.js';
import { logLine, logLines, clearLog, guard } from './ui.js';
import { call, hooks } from './serial.js';
import { decodeLog, rawLog } from './logdecode.js';

// When an entry was logged: board uptime, prefixed by its boot's number when it's from an earlier boot.
const when = (e) => (e.boot !== undefined ? `boot ${e.boot} ` : '') + fmtDur(e.t);
// A log entry (pushed as an event, or from log.get) decoded, with the board's own form kept for the download. A
// coalesced flood (n: how many times it came) says so.
export const showLogEntry = (e) => {
  const times = e.n > 1 ? ` (×${e.n})` : '';
  logLine(`[${when(e)}] ${decodeLog(e, S.meta)}${times}`, '', `[${when(e)}] ${rawLog(e)}`);
};

// The Older button's cursor: the journal record it showed last (its lseq; the next press asks for the one before), or
// null to start from the newest. A cursor, not a count, so records the board writes meanwhile don't shift the pages.
const NEWEST = 4294967295;
let olderBefore = null;
export const resetLogPaging = () => { olderBefore = null; };

export function initLog() {
  hooks.onRawLine = (line) => { if ($('logRaw').checked) logLine(line, 'raw'); };
  $('btnLogGet').onclick = guard(async () => {
    const res = await call('log.get');
    const boot = res.boot !== undefined ? `, boot ${res.boot}` : '';
    logLine(`--- board log (${res.log.length} entries${boot}, board uptime ${fmtDur(res.now)}) ---`);
    res.log.forEach(showLogEntry);
    resetLogPaging();
  });
  // The flash journal, newest record first: each press shows the next older one, its entries oldest first.
  $('btnLogOlder').onclick = guard(async () => {
    const res = await call('log.get', { before: olderBefore ?? NEWEST });
    if (res.seq === undefined && res.log.length) {  // before 0.19.0 log.get ignores `before`: that's its RAM ring
      logLine('--- this board’s firmware keeps no journal (0.19.0 on): Fetch board log shows all it has ---');
      return;
    }
    if (!res.log.length) {
      logLine('--- no older entries in the board’s journal ---');
      return;
    }
    olderBefore = res.seq;
    logLine(`--- journal record ${res.seq} (${res.log.length} entries) ---`);
    res.log.forEach(showLogEntry);
  });
  $('btnLogClear').onclick = clearLog;
  $('btnLogSave').onclick = () => {
    const stamp = new Date().toISOString().slice(0, 19).replace(/[T:]/g, '-');
    download(`gatelink-${S.role}-log-${stamp}.txt`, logLines.join('\n') + '\n', 'text/plain');
  };
}
