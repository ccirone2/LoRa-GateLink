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

let olderRec = 0;  // the journal record the Older button fetched last (0: none since Fetch)

export function initLog() {
  hooks.onRawLine = (line) => { if ($('logRaw').checked) logLine(line, 'raw'); };
  $('btnLogGet').onclick = guard(async () => {
    const res = await call('log.get');
    logLine(`--- board log (${res.log.length} entries, boot ${res.boot}, board uptime ${fmtDur(res.now)}) ---`);
    res.log.forEach(showLogEntry);
    olderRec = 0;
  });
  // The flash journal, newest record first: each press shows the next older one, its entries oldest first.
  $('btnLogOlder').onclick = guard(async () => {
    const res = await call('log.get', { rec: olderRec + 1 });
    if (!res.log.length) {
      logLine('--- no older entries in the board’s journal ---');
      return;
    }
    olderRec++;
    logLine(`--- journal record ${olderRec} (${res.log.length} entries) ---`);
    res.log.forEach(showLogEntry);
  });
  $('btnLogClear').onclick = clearLog;
  $('btnLogSave').onclick = () => {
    const stamp = new Date().toISOString().slice(0, 19).replace(/[T:]/g, '-');
    download(`gatelink-${S.role}-log-${stamp}.txt`, logLines.join('\n') + '\n', 'text/plain');
  };
}
