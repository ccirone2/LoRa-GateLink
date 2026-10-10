// The Status tab: board info, the live status (polled, and pushed by the house on each gate report), the role.
import { $, esc, cap, fmtDur, fmtNum, yesNo, pill } from './util.js';
import { S } from './state.js';
import { call } from './serial.js';
import { IO_LABELS, WIRING, renderWiring } from './wiring.js';
import { updateFwCard, reportFlash } from './firmware.js';
import { showBoardKey } from './security.js';
import { PROBLEMS } from './logdecode.js';

// Status `health` (the problems now) and `fault_out` (D5: true high, false low, null off) as HTML; firmware before
// 0.14.0 reports neither.
export function healthHtml(s) {
  if (!Array.isArray(s.health)) return '—';
  const probs = s.health.map((p) => PROBLEMS[p] ?? p);
  const now = probs.length ? `<span class="bad">${esc(probs.join(', '))}</span>` : '<span class="good">ok</span>';
  const d5 = s.fault_out === true ? '<span class="good">D5 high</span>'
    : s.fault_out === false ? '<span class="bad">D5 low</span>' : 'D5 off';
  return `${now} · ${d5}`;
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
  const hint = {
    gate: 'K1 pulses the opener OPEN input and K2 the CLOSE input — this moves the real gate.',
    house: 'K1 toggles the controller sync output (gate commands are paused during the test). K2 drives the contact sensor output.',
    unset: 'Set a role first.',
  };
  $('relayHint').textContent = hint[S.role] || '';
  $('btnK1').textContent = S.role === 'gate' ? 'Pulse K1 (OPEN)' : 'Pulse K1';
  $('btnK2').textContent = S.role === 'gate' ? 'Pulse K2 (CLOSE)' : 'Pulse K2';
  if (WIRING[S.role]) renderWiring(S.role);
}

let devline = '';

export async function refreshInfo() {
  const info = await call('info');
  S.boardInfo = info;
  updateFwCard();
  if (S.flashCheck) reportFlash(info);
  devline = `${info.board} · fw ${info.fw} · ${info.role}`;
  $('devline').textContent = devline;
  S.keySet = !!info.key_set;
  $('keyWarn').hidden = info.key_set;
  $('secKeySet').textContent = info.key_set ? 'yes' : 'no (link disabled)';
  showBoardKey();
  // Boards before 0.5.0 don't report it: program flash, erased by uploads.
  const spi = info.cfg_store === 'spi';
  $('secStore').textContent = spi ? 'flash chip (kept across firmware updates)' : 'program flash (erased by firmware updates)';
  $('secStore').className = spi ? '' : 'bad';
  applyRole(info.role);
}

// Frequency error in Hz, signed, with the crystal offset it means at our carrier (ppm).
export function fmtFei(hz) {
  if (hz === undefined || hz === null) return '—';
  const ppm = S.params.freq_hz ? ` (${(Math.abs(hz) / (S.params.freq_hz / 1e6)).toFixed(2)} ppm)` : '';
  return `${hz > 0 ? '+' : ''}${Math.round(hz)} Hz${ppm}`;
}

export async function refreshStatus(timeoutMs) {
  const res = await call('status', {}, timeoutMs);
  renderStatus(res.status);
}

// Status poll. Two misses in a row (the port is open but the board doesn't answer) dim the status cards
// like a disconnect, and the header says so, so stale values don't read as live.
let pollMisses = 0;
// The board answers status at once; a timeout under the poll interval keeps polls from piling up.
export const POLL_MS = 2000;
let polling = false;
export async function pollStatus() {
  if (polling) return;
  polling = true;
  try {
    await refreshStatus(POLL_MS - 100);
    pollMisses = 0;
  } catch {
    pollMisses++;
  } finally {
    polling = false;
  }
  if (!S.port) return;
  const stale = pollMisses >= 2;
  document.body.classList.toggle('offline', stale);
  $('devline').textContent = stale ? `${devline} · not responding` : devline;
}

export const resetPoll = () => { pollMisses = 0; };

export function renderStatus(s) {
  if (s.role !== S.role) applyRole(s.role);
  S.rebootPending = !!s.reboot_pending;
  const gs = s.gate || 'unknown';
  const g = $('gateState');
  // With the link down the house still reports the gate's last state: show it struck through, not as live.
  const stale = s.role === 'house' && s.link_up === false && gs !== 'unknown';
  g.textContent = s.role === 'unset' ? 'role not set' : gs.replaceAll('_', ' ');
  g.className = `gate-state ${stale ? 'stale' : gs}`;
  $('gateStale').hidden = !stale;
  updateTitle(s.role === 'unset' ? '' : stale ? 'Link lost' : cap(gs.replaceAll('_', ' ')));
  $('gateCause').textContent = s.cause ?? '—';
  $('gateResult').textContent = s.last_result ?? '—';
  $('gateTarget').textContent = s.target || '—';

  const l = s.link || {}; // absent from a malformed or foreign status: render what's there
  const heard = l.age_ms >= 0;
  $('lnkVerified').innerHTML = l.verified ? '<span class="good">yes</span>' : '<span class="bad">no</span>';
  $('lnkAge').textContent = heard ? `${fmtDur(l.age_ms)} ago` : 'never';
  $('lnkRssi').textContent = heard ? `${l.rssi} dBm / ${Number(l.snr).toFixed(1)} dB` : '—';
  $('lnkTxRx').textContent = `${l.tx} / ${l.rx}`;
  $('lnkRetry').textContent = `${l.retries} / ${l.giveups}`;
  $('lnkBad').textContent = `${l.mac_fail} / ${l.replay}`;
  $('lnkLbt').textContent = l.lbt_defers === undefined ? '—' : `${l.lbt_defers} / ${l.lbt_forced}`;
  // Newer fields: absent on older firmware; noise is null before its first sample, fei before the first frame.
  $('lnkCrc').textContent = l.crc_err ?? '—';
  $('lnkNoise').textContent = l.noise === undefined || l.noise === null ? '—' : `${fmtNum(l.noise)} dBm`;
  $('lnkFei').textContent = fmtFei(heard ? l.fei : null);

  const labels = IO_LABELS[s.role] || IO_LABELS.unset;
  const io = s.io || {};
  $('ioList').innerHTML = ['in1', 'in2', 'in3', 'in4', 'k1', 'k2'].filter((k) => k in io)
    .map((k) => `<div class="kv"><span>${labels[k]}</span>${pill(io[k])}</div>`).join('');

  $('bRole').textContent = s.reboot_pending ? `${s.role} (reboot to apply saved role)` : s.role;
  $('bFw').textContent = s.fw;
  $('bUp').textContent = fmtDur(s.uptime_ms);
  $('bReset').textContent = (s.reset_cause ?? '—').replaceAll('_', ' ');
  $('bSupply').innerHTML = s.supply === undefined || s.supply === null ? '—'
    : s.supply ? '<span class="good">ok</span>' : '<span class="bad">lost · on battery</span>';
  const nf = Number(s.radio_faults) || 0;
  const faults = nf ? ` <span class="bad">· ${nf} TX fault${nf === 1 ? '' : 's'}</span>` : '';
  $('bRadio').innerHTML = (s.radio_ok ? '<span class="good">ok</span>' : '<span class="bad">not initialised</span>') + faults;
  $('bHealth').innerHTML = healthHtml(s);
  $('keyWarn').hidden = s.key_set;
  $('bSettling').textContent = 'settling' in s ? (s.settling ? 'yes · first report waits' : 'no') : '—';
  const store = { spi: 'flash chip', internal: 'program flash' }[s.cfg_store] ?? s.cfg_store;
  const fid = S.boardInfo?.flash_id ? ` · id ${S.boardInfo.flash_id}` : '';
  $('bCfg').innerHTML = s.cfg_loaded === undefined ? '—'
    : `${s.cfg_loaded ? 'saved' : '<span class="bad">none · defaults</span>'}${store ? ` · ${esc(store)}` : ''}${esc(fid)}`;
  const ram = s.free_ram === undefined ? '—' : `${Number(s.free_ram)} B`;
  $('bMem').innerHTML = `${ram} / ${s.usb_cut ? `<span class="bad">${Number(s.usb_cut)}</span>` : s.usb_cut ?? '—'}`;

  if (s.role === 'house') {
    const r = s.remote || {};
    $('lnkUp').innerHTML = s.link_up ? '<span class="good">up</span>' : '<span class="bad">down</span>';
    $('lnkRemoteRssi').textContent = r.uptime_s ? `${r.rssi} dBm / ${Number(r.snr).toFixed(1)} dB` : '—';
    $('hCtrl').innerHTML = pill(s.ctrl);
    $('hCtrlPower').innerHTML = 'ctrl_power' in s ? (s.ctrl_power ? pill(true) : '<span class="bad">off · edges ignored</span>') : '—';
    $('hArmed').textContent = yesNo(s.armed);
    $('hSync').textContent = `${yesNo(s.sync_window)} / ${yesNo(s.resyncing)}`;
    const cmdRes = { '-1': 'none', '-2': 'gave up', 0: 'ok', 1: 'already there', 2: 'rejected', 4: 'refused: no AC power' };
    $('hCmd').textContent = `#${s.cmd_id} · ${s.cmd_pending ? 'sending…' : cmdRes[s.cmd_result] ?? s.cmd_result}`;
    $('hLimits').textContent = r.uptime_s ? `open ${r.open_limit ? '●' : '○'}  closed ${r.close_limit ? '●' : '○'}` : '—';
    $('hSpare').textContent = r.uptime_s && 'in3' in r ? `AC ${r.ac_power ?? r.in3 ? '●' : '○'}  IN4 ${r.in4 ? '●' : '○'}` : '—';
    $('hGateUp').textContent = r.uptime_s ? fmtDur(r.uptime_s * 1000) : '—';
    // The house waits max(link_timeout_s, 2.5 × the gate's heartbeat) before declaring the link down.
    $('hTiming').textContent = `${r.heartbeat_s ? `${r.heartbeat_s} s` : '—'} / ${s.link_timeout_eff_s ? `${s.link_timeout_eff_s} s` : '—'}`;
  }
}
