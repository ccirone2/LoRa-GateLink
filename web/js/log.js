// The Log tab's controls: the board's log events, decoded (logdecode.js), its stored log, raw serial and the
// download. The view itself is ui.js's, as every module writes to it.
import { $, fmtDur, download } from './util.js';
import { S } from './state.js';
import { logLine, logLines, clearLog, guard } from './ui.js';
import { call, hooks } from './serial.js';
import { decodeLog, rawLog } from './logdecode.js';

// A log entry (pushed as an event, or from log.get) decoded, with the board's own form kept for the download.
export const showLogEntry = (e) => logLine(`[${fmtDur(e.t)}] ${decodeLog(e, S.meta)}`, '', `[${fmtDur(e.t)}] ${rawLog(e)}`);

export function initLog() {
  hooks.onRawLine = (line) => { if ($('logRaw').checked) logLine(line, 'raw'); };
  $('btnLogGet').onclick = guard(async () => {
    const res = await call('log.get');
    logLine(`--- board log (${res.log.length} entries, board uptime ${fmtDur(res.now)}) ---`);
    res.log.forEach(showLogEntry);
  });
  $('btnLogClear').onclick = clearLog;
  $('btnLogSave').onclick = () => {
    const stamp = new Date().toISOString().slice(0, 19).replace(/[T:]/g, '-');
    download(`gatelink-${S.role}-log-${stamp}.txt`, logLines.join('\n') + '\n', 'text/plain');
  };
}
