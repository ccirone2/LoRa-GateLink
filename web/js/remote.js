// The house's remote gate card: the gate's diagnostics and remote writes of its settings, over the link.
import { $, fmtDur } from './util.js';
import { S } from './state.js';
import { guard } from './ui.js';
import { call } from './serial.js';
import { rememberGateFw } from './peer.js';

// DIAG over LoRa is best effort; give up on the reply after this long.
const DIAG_TIMEOUT_MS = 10000;
let diagTimer = null;

async function remoteDiag() {
  clearTimeout(diagTimer);
  $('diagOut').textContent = 'waiting for gate…';
  // Armed before the request: the remote_diag event clears it, however soon it arrives.
  diagTimer = setTimeout(() => {
    $('diagOut').textContent = `No reply from gate within ${DIAG_TIMEOUT_MS / 1000} s. Check the link and try again.`;
  }, DIAG_TIMEOUT_MS);
  try {
    await call('remote.diag');
  } catch (e) {
    clearTimeout(diagTimer);
    $('diagOut').textContent = `Failed: ${e.message}`;
    throw e;
  }
}

// The remote-writable settings, from the board's config.get meta, so firmware changes to P_REMOTE follow
// automatically.
export function fillRemoteParams() {
  const sel = $('remParam');
  const keep = sel.value;
  sel.replaceChildren(...S.meta.filter((m) => m.remote).map((m) => new Option(m.name, m.name)));
  if (keep && S.meta.some((m) => m.remote && m.name === keep)) sel.value = keep;
  syncRemValue();
}

// Show the selected remote param's range on the value box (the board still validates it).
function syncRemValue() {
  const m = S.meta.find((x) => x.name === $('remParam').value);
  const el = $('remValue');
  if (!m) return;
  el.min = m.min;
  el.max = m.max;
  el.placeholder = `${m.min}–${m.max}`;
}

async function remoteSet() {
  const name = $('remParam').value;
  const raw = $('remValue').value.trim();
  const value = Number(raw);
  if (raw === '' || !Number.isInteger(value)) {
    $('remResult').textContent = 'Enter an integer value.';
    return;
  }
  $('remResult').textContent = 'sending…';
  try {
    await call('remote.set', { name, value });
  } catch (e) {
    // e.g. "busy" while the previous remote set is still pending, or an out-of-range value.
    $('remResult').textContent = e.message === 'busy' ? 'Busy: the previous remote set is still pending; wait for its result.' : `Not sent: ${e.message}`;
    throw e;
  }
}

// The remote_diag and remote_set events.
export function onRemoteEvent(ev) {
  if (ev.event === 'remote_diag') {
    clearTimeout(diagTimer);
    rememberGateFw(ev.fw);
    const c = ev.counters;
    $('diagOut').textContent =
      `fw ${ev.fw} · up ${fmtDur(ev.uptime_s * 1000)}\n` +
      `tx ${c.tx} rx ${c.rx} retries ${c.retries} giveups ${c.giveups} mac_fail ${c.mac_fail} replay ${c.replay}\n` +
      Object.entries(ev.params).map(([k, v]) => `${k}=${v}`).join('  ');
  } else if (ev.event === 'remote_set') {
    $('remResult').textContent = ev.ok ? 'Gate accepted and saved the value.'
      : ev.applied ? 'Gate applied the value but couldn’t save it; it reverts when the gate reboots.'
      : ev.acked ? 'Gate rejected the value.' : 'No reply from gate.';
  }
}

// Another board connected: these results were the last one's.
export function resetRemote() {
  $('diagOut').textContent = '—';
  $('remResult').textContent = '';
}

export const stopRemote = () => clearTimeout(diagTimer);

export function initRemote() {
  $('btnDiag').onclick = guard(remoteDiag);
  $('remParam').onchange = syncRemValue;
  $('remValue').addEventListener('keydown', (e) => { if (e.key === 'Enter' && !$('btnRemSet').disabled) $('btnRemSet').click(); });
  $('btnRemSet').onclick = guard(remoteSet);
}
