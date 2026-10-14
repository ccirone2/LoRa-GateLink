// GateLink web console: talks newline-delimited JSON to the board over Web Serial (docs/console.md). Plain ES
// modules, no build step. This file starts the page and runs the connection; each card's module wires its own
// controls (its init function). In js/:
//   serial.js   port, requests, board picker, reconnect     board.js    the board's info, role, header line, title
//   status.js   Status tab                                   config.js   Config tab (settings.js: groups, help text)
//   log.js      Log tab (logdecode.js: events as text)       peer.js     the other board, as last seen
//   Link tab:   history.js, surveycard.js (survey.js: the verdict, twin of tools/gatelink_client/survey.py),
//               ping.js, remote.js (the gate over the link)
//   Board tab:  security.js (keys.js: key ids, backups), firmware.js (samba.js: bootloader)
//   Install tab: wiring.js (tracks pins.h), relay.js
//   healthline.js the Status tab's verdict   ui.js log view, toasts   state.js shared state   util.js helpers
import { $ } from './js/util.js';
import { S } from './js/state.js';
import { worksOffline, toast, hideToast, guard } from './js/ui.js';
import * as serial from './js/serial.js';
import { applyRole, boardGone, refreshInfo } from './js/board.js';
import { refreshStatus, renderStatus, pollStatus, resetPoll, showHealthLine, POLL_MS } from './js/status.js';
import { resetPeer } from './js/peer.js';
import { initConfig, loadConfig, markUnsaved, dirtyCount } from './js/config.js';
import { initSecurity } from './js/security.js';
import { initLog, showLogEntry } from './js/log.js';
import { initWiring } from './js/wiring.js';
import { initPing, onPong, resetPing, stopPing } from './js/ping.js';
import { initSurvey, surveyPong, resetSurvey, stopSurvey } from './js/surveycard.js';
import { initRemote, onRemoteEvent, resetRemote, stopRemote } from './js/remote.js';
import { initRelay } from './js/relay.js';
import { initHistory, clearHistoryView } from './js/history.js';
import { initFirmware } from './js/firmware.js';

let pollTimer = null;

function setConnected(on) {
  if (!on) showHealthLine(null);
  $('btnConnect').hidden = on;
  $('btnDisconnect').hidden = !on;
  $('btnIdentify').hidden = !on;
  document.querySelectorAll('main button, main input, main select').forEach((el) => { if (!worksOffline(el)) el.disabled = !on; });
  document.body.classList.toggle('offline', !on);
  resetPoll();
  if (!on) {
    markUnsaved(false); // a reboot drops them; another board never had them
    clearHistoryView(); // another board (or this one after a reset) has a different record
    boardGone();
  }
}

// Unsolicited events from the board, to the cards that wait for them.
const EVENTS = {
  log: showLogEntry,
  status: (ev) => renderStatus(ev.status),
  pong: (ev) => { surveyPong(ev); onPong(ev); },
  remote_diag: onRemoteEvent,
  remote_set: onRemoteEvent,
};

serial.hooks.onOpen = async ({ otherBoard, port }) => {
  if (otherBoard) { // another board: its ping, survey and diagnostics results don't apply
    resetPing();
    resetSurvey();
    resetRemote();
  }
  resetPeer();
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
  stopSurvey();
  stopPing();
  stopRemote();
};
serial.hooks.onClosed = () => setConnected(false);
serial.hooks.onEvent = (ev) => { if (Object.hasOwn(EVENTS, ev.event)) EVENTS[ev.event](ev); };

// Tabs that were split up (2026-10), so their links and bookmarks still land somewhere sensible.
const OLD_TABS = { tools: 'link', security: 'board' };

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
    const h = location.hash.slice(1);
    const name = Object.hasOwn(OLD_TABS, h) ? OLD_TABS[h] : h;
    const b = tabs.find((x) => x.dataset.tab === name);
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
  serial.initSerial();
  initWiring();
  initConfig();
  initSecurity();
  initRelay();
  initPing();
  initSurvey();
  initRemote();
  initHistory();
  initFirmware();
  initLog();
  setConnected(false);
  applyRole('unset');
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
  $('btnDisconnect').onclick = () => { serial.stopReconnect(); serial.disconnect(); };
  $('btnIdentify').onclick = guard(async () => { await serial.call('identify'); toast('LED strobing for 6 s.'); });
}

init();
