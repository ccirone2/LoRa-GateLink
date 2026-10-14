// The relay test card: pulse K1 or K2 for a set time (relay.test).
import { $ } from './util.js';
import { S } from './state.js';
import { toast, guard } from './ui.js';
import { call } from './serial.js';

const HINT = {
  gate: 'K1 pulses the opener OPEN input and K2 the CLOSE input — this moves the real gate.',
  house: 'K1 toggles the controller sync output (gate commands are paused during the test). K2 drives the contact sensor output.',
  unset: 'Set a role first.',
};

async function relayTest(k) {
  const ms = Number($('relayMs').value);
  if (!Number.isInteger(ms) || ms < 50 || ms > 5000) return toast('Pulse length must be 50–5000 ms.', 'err');
  if (S.role === 'gate' && !confirm(`This will pulse the opener ${k === 1 ? 'OPEN' : 'CLOSE'} input and move the gate. Continue?`)) return;
  await call('relay.test', { k, ms });
  toast(`K${k} pulsed for ${ms} ms.`);
}

// The board's role: what the relays are wired to.
export function showRelayRole(role) {
  $('relayHint').textContent = HINT[role] || '';
  $('btnK1').textContent = role === 'gate' ? 'Pulse K1 (OPEN)' : 'Pulse K1';
  $('btnK2').textContent = role === 'gate' ? 'Pulse K2 (CLOSE)' : 'Pulse K2';
}

export function initRelay() {
  $('btnK1').onclick = guard(() => relayTest(1));
  $('btnK2').onclick = guard(() => relayTest(2));
}
