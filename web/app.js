// GateLink web console: talks newline-delimited JSON to the board over Web Serial (docs/console.md). Plain ES
// modules, no build step. This file wires the page together; the work is in js/:
//   serial.js   port, requests, board picker, reconnect      status.js   Status tab
//   config.js   Config tab                                    tools.js    ping, diagnostics, remote writes, relays
//   history.js  link history chart                            firmware.js firmware update (samba.js: bootloader)
//   wiring.js   Install tab (tracks pins.h)                   settings.js setting groups and help text
//   security.js Security tab (keys.js: key ids, backups)      logdecode.js log events as text
//   ui.js log view, toasts      state.js shared state      util.js helpers
import { $, fmtDur, download } from './js/util.js';
import { S } from './js/state.js';
import { OFFLINE_OK, logLine, logLines, clearLog, toast, hideToast, guard } from './js/ui.js';
import * as serial from './js/serial.js';
import { decodeLog, rawLog } from './js/logdecode.js';
import { renderWiring, wiringRole, wiringResized } from './js/wiring.js';
import { applyRole, refreshInfo, refreshStatus, renderStatus, pollStatus, resetPoll, POLL_MS } from './js/status.js';
import {
  loadConfig, syncRemValue, applyConfig, importConfig, exportConfig, reloadConfig, saveConfig, resetConfig,
  markUnsaved, dirtyCount,
} from './js/config.js';
import { loadHistory, clearHistoryView, historyCsv, historyRole, historyLoaded, initHistory } from './js/history.js';
import { updateFwCard, loadLatest, installLatest, flashFile, pickBootPort } from './js/firmware.js';
import {
  ping, setAutoPing, resetToolsView, stopTools, relayTest, remoteDiag, remoteSet, replay, onToolsEvent,
} from './js/tools.js';
import {
  checkKeyInput, showBoardKey, generate, copyKey, keyToWrite, reportKeyWritten, saveBackup, restoreBackup,
} from './js/security.js';

let pollTimer = null;

function setConnected(on) {
  $('btnConnect').hidden = on;
  $('btnDisconnect').hidden = !on;
  $('btnIdentify').hidden = !on;
  document.querySelectorAll('main button, main input, main select').forEach((el) => {
    if (el.closest('#tab-install') || el.classList.contains('info')) return; // reference, usable without a board
    if (!OFFLINE_OK.includes(el.id)) el.disabled = !on;
  });
  document.body.classList.toggle('offline', !on);
  resetPoll();
  if (!on) markUnsaved(false); // a reboot drops them; another board never had them
  if (!on) clearHistoryView(); // another board (or this one after a reset) has a different record
  $('btnHistCsv').disabled = !historyLoaded();
  if (!on) {
    S.boardInfo = null;
    showBoardKey();
    updateFwCard();
    $('devline').textContent = 'not connected';
    $('keyWarn').hidden = true;
    document.title = 'GateLink Console';
  }
}

const logEntryLine = (e) => `[${fmtDur(e.t)}] ${decodeLog(e, S.meta)}`;

function onEvent(ev) {
  switch (ev.event) {
    case 'log':
      logLine(logEntryLine(ev), '', `[${fmtDur(ev.t)}] ${rawLog(ev)}`);
      break;
    case 'status':
      renderStatus(ev.status);
      break;
    default:
      onToolsEvent(ev);
  }
}

serial.hooks.onOpen = async ({ otherBoard, port }) => {
  if (otherBoard) resetToolsView(); // another board: its ping/diag results don't apply
  setConnected(true);
  try {
    await refreshInfo();
    await loadConfig();
    await refreshStatus();
  } catch (e) {
    toast(`Connected, but the board didn't answer: ${e.message}`, 'err');
  }
  if (S.port !== port) return; // dropped while querying: disconnect() already ran; don't leave an orphan poll
  clearInterval(pollTimer);
  pollTimer = setInterval(pollStatus, POLL_MS);
};
serial.hooks.onClosing = () => {
  clearInterval(pollTimer);
  stopTools();
};
serial.hooks.onClosed = () => setConnected(false);
serial.hooks.onEvent = onEvent;
serial.hooks.onRawLine = (line) => { if ($('logRaw').checked) logLine(line, 'raw'); };

// Tabs: click or arrow keys (roving tabindex, per the ARIA tabs pattern).
function initTabs() {
  const tabs = [...document.querySelectorAll('#tabs [role=tab]')];
  const selectTab = (b) => {
    tabs.forEach((x) => {
      const on = x === b;
      x.classList.toggle('active', on);
      x.setAttribute('aria-selected', String(on));
      x.tabIndex = on ? 0 : -1;
    });
    document.querySelectorAll('.tab').forEach((t) => t.classList.toggle('active', t.id === `tab-${b.dataset.tab}`));
    history.replaceState(null, '', `#${b.dataset.tab}`); // survives a page reload
  };
  tabs.forEach((b, i) => {
    b.addEventListener('click', () => selectTab(b));
    b.addEventListener('keydown', (e) => {
      const j = { ArrowRight: i + 1, ArrowLeft: i - 1, Home: 0, End: tabs.length - 1 }[e.key];
      if (j === undefined) return;
      e.preventDefault();
      const next = tabs[(j + tabs.length) % tabs.length];
      selectTab(next);
      next.focus();
    });
  });
  const tabFromHash = () => {
    const b = tabs.find((x) => `#${x.dataset.tab}` === location.hash);
    if (b) selectTab(b);
  };
  tabFromHash();
  window.addEventListener('hashchange', tabFromHash); // back/forward, or a #tab link
}

function init() {
  if (!('serial' in navigator)) {
    // Phones and tablets have no Web Serial in any browser.
    const ua = navigator.userAgent;
    const mobile = navigator.userAgentData?.mobile || /Android|iPhone|iPad|iPod|Mobile/i.test(ua)
      || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1); // iPadOS reports a desktop Safari
    if (mobile) $('unsupportedMsg').textContent = 'Phones and tablets can’t talk to the board: browsers there have no Web Serial. '
      + 'Use a desktop or laptop with Chrome or Edge and a USB cable. The Install tab still works here.';
    $('unsupported').hidden = false;
    $('btnConnect').disabled = true;
  }
  setConnected(false);
  renderWiring(wiringRole());
  applyRole('unset');
  document.querySelectorAll('[data-wiring]').forEach((b) => b.addEventListener('click', () => renderWiring(b.dataset.wiring)));
  new ResizeObserver(([e]) => wiringResized(e.contentRect.width)).observe(document.querySelector('.wiring'));
  initTabs();

  $('toast').onclick = hideToast;
  // Chrome steps a focused number field on mouse wheel: scrolling the page past one (freq_hz, say) would
  // silently change it. Drop focus instead, so the wheel scrolls the page.
  document.addEventListener('wheel', (e) => {
    if (e.target instanceof HTMLInputElement && e.target.type === 'number' && e.target === document.activeElement) e.target.blur();
  }, { passive: true });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && $('toast').textContent) hideToast(); });
  // Unapplied form edits are lost on reload/close.
  window.addEventListener('beforeunload', (e) => { if (S.flashing || (S.meta.length && dirtyCount())) e.preventDefault(); });

  $('btnConnect').onclick = serial.connect;
  $('btnPickAdd').onclick = serial.addBoard;
  $('btnPickRefresh').onclick = serial.pickRefresh;
  $('btnPickCancel').onclick = serial.closePicker;
  $('boardPicker').addEventListener('close', serial.pickerClosed);
  $('boardPicker').addEventListener('click', (e) => { // backdrop clicks land on the dialog too, outside its box
    const r = $('boardPicker').getBoundingClientRect();
    if (e.target === $('boardPicker') && (e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom)) {
      serial.closePicker();
    }
  });
  $('btnDisconnect').onclick = () => { serial.stopReconnect(); serial.disconnect(); };
  $('btnIdentify').onclick = guard(async () => { await serial.call('identify'); toast('LED strobing for 6 s.'); });

  $('btnCfgLoad').onclick = guard(reloadConfig);
  $('btnCfgApply').onclick = guard(applyConfig);
  // Setting tooltips show on hover or focus; a tap or click pins one open (touch has no hover).
  const closeTips = (except) => document.querySelectorAll('.tip.show').forEach((t) => t !== except && t.classList.remove('show'));
  document.addEventListener('click', (e) => {
    const info = e.target.closest('.info');
    const tip = info?.nextElementSibling;
    closeTips(tip);
    tip?.classList.toggle('show');
  });
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape') return;
    closeTips();
    if (document.activeElement?.classList.contains('info')) document.activeElement.blur();
  });
  $('btnCfgSave').onclick = guard(saveConfig);
  $('btnReboot').onclick = guard(async () => {
    const lose = S.appliedUnsaved ? '\n\nApplied settings haven’t been saved to flash and will be lost.' : '';
    if (!confirm(`Reboot the board? Relays release during reboot.${lose}`)) return;
    await serial.rebootAndReconnect();
    toast('Board rebooting — reconnecting…');
  });
  $('btnCfgReset').onclick = guard(resetConfig);
  $('btnCfgExport').onclick = exportConfig;
  $('fileImport').onchange = (e) => { if (e.target.files[0]) importConfig(e.target.files[0]); e.target.value = ''; };

  // Key input: accepts pasted keys with spaces, colons or dashes; the hint flags anything that isn't 32 hex chars, or
  // gives the key's id.
  $('btnKeyGen').onclick = generate;
  $('keyInput').addEventListener('input', checkKeyInput);
  $('keyInput').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !$('btnKeySet').disabled) $('btnKeySet').click(); });
  $('btnKeyCopy').onclick = copyKey;
  $('btnKeySet').onclick = guard(async () => {
    const key = await keyToWrite();
    if (!key) return;
    const same = S.boardInfo?.key_id === key.id;
    if (S.keySet && !confirm(same ? `This board already has this key (id ${key.id}). Write it again?`
      : 'This board already has a key. Replacing it takes the link down until the other board gets the same key. Continue?')) return;
    await serial.call('key.set', { key: key.hex });
    await refreshInfo();
    reportKeyWritten(key.id);
  });
  $('btnKeyBackup').onclick = guard(saveBackup);
  $('keyRestoreFile').onchange = guard(async (e) => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) await restoreBackup(f);
  });

  $('btnK1').onclick = guard(() => relayTest(1));
  $('btnK2').onclick = guard(() => relayTest(2));
  $('btnPing').onclick = guard(ping);
  $('pingAuto').onchange = (e) => setAutoPing(e.target.checked);
  $('btnDiag').onclick = guard(remoteDiag);
  $('remParam').onchange = syncRemValue;
  $('remValue').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !$('btnRemSet').disabled) $('btnRemSet').click(); });
  $('btnRemSet').onclick = guard(remoteSet);
  $('btnReplay').onclick = guard(replay);

  $('btnHistLoad').onclick = guard(async () => {
    await loadHistory();
    $('btnHistLoad').textContent = 'Refresh';
  });
  $('btnHistCsv').onclick = () => historyLoaded() && download(`gatelink-${historyRole()}-link-history.csv`, historyCsv(), 'text/csv');
  $('btnHistClear').onclick = guard(async () => {
    if (!confirm('Empty this board’s link history and start recording again from now?')) return;
    await serial.call('hist.clear');
    await loadHistory();
    toast('Link history cleared.');
  });
  initHistory();

  loadLatest();
  $('btnFwLatest').onclick = guard(installLatest);
  $('fwFile').onchange = guard(async (e) => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) await flashFile(f);
  });
  $('btnBootPort').onclick = pickBootPort;

  $('btnLogGet').onclick = guard(async () => {
    const res = await serial.call('log.get');
    logLine(`--- board log (${res.log.length} entries, board uptime ${fmtDur(res.now)}) ---`);
    for (const e of res.log) logLine(logEntryLine(e), '', `[${fmtDur(e.t)}] ${rawLog(e)}`);
  });
  $('btnLogClear').onclick = clearLog;
  $('btnLogSave').onclick = () => {
    const stamp = new Date().toISOString().slice(0, 19).replace(/[T:]/g, '-');
    download(`gatelink-${S.role}-log-${stamp}.txt`, logLines.join('\n') + '\n', 'text/plain');
  };

  serial.watchPorts();
}

init();
