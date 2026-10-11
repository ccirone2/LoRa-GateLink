// Board log events (docs/console.md "Log events", log.h) as readable text for the Log tab. The downloaded log keeps
// the board's own form (`gate_state a=3 b=1`). An event this table doesn't know is shown as it came.

export const STATES = ['unknown', 'closed', 'open', 'between', 'fault', 'no_power']; // GateState
export const CAUSES = ['none', 'lora', 'external']; // Cause
const ACTIONS = { 1: 'OPEN', 2: 'CLOSE' };
const MSG_TYPES = { 1: 'HELLO', 2: 'HELLO_ACK', 3: 'ACK', 4: 'CMD', 5: 'STATUS', 6: 'PING', 7: 'PONG', 8: 'DIAG_REQ', 9: 'DIAG',
  10: 'CFG_SET' };
const ROLES = { 0: 'unset', 1: 'house', 2: 'gate' };
const CFG_SOURCES = { 0: 'defaults (nothing saved)', 1: 'the flash chip', 2: 'program flash' };
const RADIO_FAIL = { 0: 'radio failed to initialise', 1: 'radio TX fault, re-initialised', 2: 'radio reset itself, re-initialised',
  3: 'radio initialised on retry' };
const CMD_HOLD = { 0: 'held command sent after all: the gate’s session answered', 1: 'command held: HELLO from an unverified gate session',
  2: 'held command dropped: the gate restarted (it may have run it)' };
// The board's health problems (health.h Problem), in bit order: status `health` names them, log `health` b has the bits.
export const PROBLEMS = { starting: 'starting up', radio: 'radio down', link: 'link down', ac: 'no AC power at the gate',
  no_power: 'gate has no power', fault: 'gate limit fault' };
const problems = (bits) => Object.values(PROBLEMS).filter((_, i) => bits & (1 << i)).join(', ');
const FAULT_OUT = { 1: 'fault output high: healthy', 0: 'fault output low: needs attention', '-1': 'fault output off (D5 an input again)' };

const state = (v) => (STATES[v] ?? `state ${v}`).replaceAll('_', ' ');
const action = (v) => ACTIONS[v] ?? `action ${v}`;
const type = (v) => MSG_TYPES[v] ?? `type ${v}`;
const level = (v) => (v ? 'on' : 'off');
const hex = (v) => (v >>> 0).toString(16).padStart(8, '0');

// PM->RCAUSE bits, named as the firmware does (resetCauseName in app.cpp).
export function resetCause(rc) {
  if (rc & 0x20) return 'watchdog';
  if (rc & 0x06) return 'brownout';
  if (rc & 0x01) return 'power-on';
  if (rc & 0x10) return 'reset pin';
  if (rc & 0x40) return 'software';
  return 'unknown';
}

const DECODE = {
  boot: (a, b) => `booted (${resetCause(a)} reset), role ${ROLES[b] ?? b}`,
  radio_fail: (a, b) => `${RADIO_FAIL[a] ?? `radio fault ${a}`} (faults ${b})`,
  link_up: () => 'link up',
  link_down: () => 'link down: no status report from the gate within the link timeout',
  session: (a) => `peer session ${hex(a)} verified`,
  mac_fail: (a, b) => `frame failed authentication (${type(a)}, ${b} dBm)`,
  replay: (a, b) => `replayed or repeated frame rejected (seq ${a >>> 0}, last ${b >>> 0})`,
  tx_giveup: (a, b) => `gave up sending ${type(a)} (seq ${b >>> 0}): no acknowledgement`,
  cmd_sent: (a, b) => `${action(a)} command sent (#${b})`,
  cmd_suppressed: (a, b) => `${action(a)} not sent: the gate is already ${state(b)}`,
  cmd_dropped: (a, b) => `${action(a)} command dropped (#${b}): not acknowledged in time, or the link restarted`,
  cmd_rx: (a, b) => `${action(a)} command received (#${b})`,
  cmd_dup: (a) => `repeated command #${a} ignored`,
  cmd_refused: (a, b) => `${action(a)} command refused (#${b}): no AC power at the gate`,
  pulse: (a, b) => `K${a} pulsed for ${b} ms`,
  gate_state: (a, b) => `gate ${state(a)}${b ? ` (${CAUSES[b] ?? b})` : ''}`,
  travel_timeout: (a) => `gate didn’t reach ${state(a)} within the travel timeout`,
  ctrl: (a, b) => `controller switched ${level(a)}${b ? ' (ignored: controller unpowered)' : ''}`,
  ctrl_power: (a, b) => `controller power ${a ? 'back' : 'lost'}${b ? `; pending ${action(b)} discarded` : ''}`,
  sync: (a) => `controller followed K1 (${level(a)}): not a command`,
  resync: (a) => `resyncing the controller to ${level(a)}`,
  cfg_remote: (a, b, name) => `remote write: ${name(a)} = ${b}`,
  input: (a, b) => `IN${a} ${level(b)}`,
  lbt_forced: (a, b) => `${type(a)} sent anyway after waiting ${b} ms for the channel`,
  cfg: (a, b) => `config loaded from ${CFG_SOURCES[a] ?? a}${b ? `; ${b} saved setting${b === 1 ? '' : 's'} dropped` : ''}`,
  supply: (a) => (a < 0 ? 'charger not answering: board supply unknown' : a ? 'board supply good' : 'board supply lost: running on the LiPo'),
  cmd_hold: (a, b) => `${CMD_HOLD[a] ?? `command hold ${a}`} (session ${hex(b)})`,
  health: (a, b) => `${FAULT_OUT[a] ?? `fault output ${a}`}${b ? ` (${problems(b)})` : ''}`,
};

export const KNOWN_EVENTS = Object.keys(DECODE);

// One log entry ({ev, a, b}) as text. meta (config.get's) names the params of remote writes.
export function decodeLog(e, meta = []) {
  const f = DECODE[e.ev];
  if (!f) return `${e.ev} a=${e.a} b=${e.b}`;
  const name = (id) => meta.find((m) => m.id === id)?.name ?? `param ${id}`;
  return f(e.a, e.b, name);
}

// The board's own form, kept in the downloaded log.
export const rawLog = (e) => `${e.ev} a=${e.a} b=${e.b}`;
