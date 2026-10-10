// The log view, toasts, and the click-handler guard.
import { $ } from './util.js';
import { S } from './state.js';

// Controls that work without a board.
export const OFFLINE_OK = ['logRaw', 'btnLogClear', 'btnLogSave', 'keyInput', 'btnKeyGen', 'btnKeyCopy', 'btnKeyBackup',
  'keyRestoreFile', 'btnFwLatest', 'fwFile', 'btnBootPort'];

export const logLines = []; // what Download saves; the view keeps only the last 1000
const LOG_KEEP = 20000;

// Adds a line to the Log tab. `raw`, if given, is what the downloaded log records instead (the board's own form of
// a decoded line), and shows as the line's tooltip.
export function logLine(text, cls = '', raw = null) {
  const stamp = new Date().toLocaleTimeString();
  logLines.push(`${stamp} ${raw ?? text}`);
  if (logLines.length > LOG_KEEP) logLines.splice(0, logLines.length - LOG_KEEP);
  const div = document.createElement('div');
  div.textContent = `${stamp} ${text}`;
  if (raw) div.title = raw;
  if (cls) div.className = cls;
  const view = $('logView');
  const atBottom = view.scrollTop + view.clientHeight >= view.scrollHeight - 20;
  view.appendChild(div);
  while (view.childElementCount > 1000) view.firstChild.remove();
  if (atBottom) view.scrollTop = view.scrollHeight;
}

export function clearLog() {
  $('logView').innerHTML = '';
  logLines.length = 0;
}

// Non-modal notice in a role="status" region (always in the DOM so screen readers announce it; empty =
// invisible). Errors stay up longer. Click to dismiss.
let toastTimer = null;
export function hideToast() {
  clearTimeout(toastTimer);
  $('toast').textContent = '';
}
export function toast(msg, kind = '') {
  logLine(msg, kind === 'err' ? 'err' : '');
  const t = $('toast');
  t.textContent = msg;
  t.className = `toast ${kind}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(hideToast, kind === 'err' ? 12000 : 5000);
}

// Runs a click handler, reporting errors as a toast. The clicked button is disabled until the handler
// finishes so a slow board reply can't be double-submitted.
export function guard(fn) {
  return async (...a) => {
    const b = a[0]?.currentTarget instanceof HTMLButtonElement ? a[0].currentTarget : null;
    if (b) b.disabled = true;
    try { await fn(...a); } catch (e) { toast(e.message, 'err'); } finally {
      if (b) b.disabled = !S.port && !OFFLINE_OK.includes(b.id);
    }
  };
}
