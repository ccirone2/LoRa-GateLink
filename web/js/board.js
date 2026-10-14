// The connected board as a whole: its info reply, its role, the header line and the page title. Cards that show
// something from the info reply register with onInfo: they import refreshInfo from here, so it can't call them.
import { $, cap } from './util.js';
import { S } from './state.js';
import { call } from './serial.js';
import { showWiringFor } from './wiring.js';
import { showRelayRole } from './relay.js';

const infoListeners = [];
// fn(info): a fresh info reply, or null once the board is gone.
export const onInfo = (fn) => infoListeners.push(fn);

let devline = '';

export async function refreshInfo() {
  const info = await call('info');
  S.boardInfo = info;
  devline = `${info.board} · fw ${info.fw} · ${info.role}`;
  $('devline').textContent = devline;
  $('keyWarn').hidden = info.key_set;
  infoListeners.forEach((fn) => fn(info));
  applyRole(info.role);
}

// The board went away: nothing on the page is its any more.
export function boardGone() {
  S.boardInfo = null;
  devline = '';
  $('devline').textContent = 'not connected';
  $('keyWarn').hidden = true;
  infoListeners.forEach((fn) => fn(null));
  document.title = 'GateLink Console';
}

// The header line, marked when the open port stops answering.
export function showResponding(ok) {
  $('devline').textContent = ok ? devline : `${devline} · not responding`;
}

// Tab title: the gate state first, so a background tab still shows it.
export function updateTitle(gate) {
  const parts = [gate, S.role !== 'unset' && cap(S.role), 'GateLink'].filter(Boolean);
  document.title = parts.length > 1 ? parts.join(' · ') : 'GateLink Console';
}

export function applyRole(r) {
  S.role = r;
  updateTitle();
  document.querySelectorAll('[data-role]').forEach((el) => { el.hidden = el.dataset.role !== S.role; });
  showRelayRole(r);
  showWiringFor(r);
}
