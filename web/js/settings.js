// Presentation of the board's settings on the Config tab: grouping, help text and the ones both boards must share.
// The settings themselves (names, ids, ranges) come from the board's config.get meta.
export const GROUPS = [
  ['General', ['role', 'net_id']],
  ['Radio', ['freq_hz', 'sf', 'bw_hz', 'cr', 'tx_power', 'sync_word']],
  ['Link', ['retries', 'heartbeat_s', 'link_timeout_s', 'cmd_ttl_s']],
  ['Inputs', ['debounce_ms', 'in1_invert', 'in2_invert', 'in3_invert', 'in4_invert', 'power_sense']],
  ['Gate node', ['pulse_ms', 'travel_timeout_s']],
  ['House node', ['ctrl_sync', 'sync_window_ms', 'resync_ms', 'mismatch_timeout_s', 'sensor_invert', 'linkloss_open',
    'ctrl_power_sense', 'ctrl_power_pmic', 'ctrl_confirm_ms', 'ctrl_settle_ms']],
  ['Fault output', ['fault_out', 'fault_hold_s']],
  ['Board', ['uart_console']],
];
// Config tooltips: what the setting does, then when you'd change it.
const NO_INVERT = 'Leave off. Inverted, a dead opto or cut wire reads as active; fix a reversed signal in the wiring instead.';
export const HELP = {
  role: 'Which end this board is: house (next to the controller) or gate (at the opener). Takes effect after Save and reboot.',
  net_id: 'Both boards must match. Change it only if another GateLink pair shares the channel nearby.',
  freq_hz: 'Both boards must match. US 902–928 MHz, EU 863–870 MHz. Move off the default only if Link history shows a high noise floor.',
  sf: 'Both boards must match. Raise it a step at a time if the link is weak or drops; each step roughly doubles airtime, so replies get slower. Lower it for faster replies on a strong link.',
  bw_hz: 'Both boards must match. Keep 500 kHz in the US (keeps a single-channel link within FCC rules). Narrower reaches further but every frame takes longer.',
  cr: 'Both boards must match. Extra error correction, 4/5 to 4/8. Raise it if Link history shows CRC errors with a decent signal; frames get longer.',
  tx_power: 'Transmit power in dBm; may differ between the boards. Lower it on USB power or at short range (full power can brown out a weak supply); raise it if the link is marginal.',
  sync_word: 'Both boards must match. Rarely changed: it filters out other LoRa networks on the same channel.',
  retries: 'Resends of an unacknowledged message, spread over its lifetime. Raise it on a lossy link; lower it to keep the channel quieter.',
  heartbeat_s: 'Gate: how often it reports status when nothing changes. Shorter spots a dead link sooner but uses more airtime. The house waits at least 2.5 heartbeats before calling the link lost.',
  link_timeout_s: 'House: no status report from the gate this long = link lost (the contact sensor then reads open). Raise it if short dropouts cause false alarms; it is never shorter than 2.5 gate heartbeats. Gate: nothing heard from the house this long (at least 2.5 of its own heartbeats) = link down, for its LED and fault output.',
  cmd_ttl_s: 'House: how long a gate command keeps being retried before it is dropped (never fired late). Raise it if commands give up during short dropouts; lower it so a stale command isn’t delivered seconds later.',
  debounce_ms: 'How long an input must hold steady before it counts. Raise it if long field wires or a bouncy contact show up as flicker in the log.',
  in1_invert: `IN1 (house: controller output; gate: open limit). ${NO_INVERT}`,
  in2_invert: `IN2 (house: controller power; gate: closed limit). ${NO_INVERT}`,
  in3_invert: `IN3 (house: spare; gate: AC power). ${NO_INVERT}`,
  in4_invert: `IN4 (spare on both boards). ${NO_INVERT}`,
  power_sense: 'Gate: IN3 watches the AC-powered 24 V supply. Without AC, commands are refused; a limit that still reads (the opener runs on its battery) is trusted, and with none the gate reads “no power” instead of between. Turn off only if IN3 isn’t wired.',
  pulse_ms: 'Gate: how long the OPEN/CLOSE contact closes. Raise it if the opener misses short presses; keep it short, as other devices share these inputs.',
  travel_timeout_s: 'Longest a full open or close should take. Set it a little above your gate’s real travel time; past it, the gate counts as stuck. Set it on the gate (here or with a remote write): the house follows the gate’s value.',
  ctrl_sync: 'House: K1 drives the controller’s switch input so the controller always shows the real gate state. Turn off only if the controller has no switch input.',
  sync_window_ms: 'House: after K1 changes, controller changes count as its echo, not a command, for this long. Raise it if a slow controller’s echo turns into an unwanted gate command.',
  resync_ms: 'House: how long K1 is released when re-syncing a controller that is out of step. Raise it if the controller doesn’t notice a short blip.',
  mismatch_timeout_s: 'House: how long the controller may disagree with the gate before K1 re-syncs it (at once after boot or power return). Lower it for faster correction; raise it if re-syncs fight someone using the controller.',
  sensor_invert: 'House: flips the contact sensor output (K2). Use it if the alarm shows open while the gate is closed.',
  linkloss_open: 'House: the contact sensor reads open while the link is lost, so the alarm never trusts a stale “closed”. Turn off only if dropouts cause too many false alerts.',
  ctrl_power_sense: 'House: IN2 watches the controller’s supply, so a power cut (which drops its relay) isn’t mistaken for a close command. Turn off only if IN2 isn’t wired.',
  ctrl_power_pmic: 'House: the board’s own supply counts as controller power too, since both run off the same supply (the board through its 5 V converter). It notices a cut or a short dip within milliseconds, before the controller’s relay drops, where the IN2 opto lags ~1.7 s and can miss short dips. Turn off if the board has its own supply, or while it runs on a USB cable that carries power.',
  ctrl_confirm_ms: 'House: a controller switch-OFF waits this long before it becomes a CLOSE, so one caused by the controller losing power (its relay drops before the power sense notices) can be discarded. Switch-ON (OPEN) goes at once: a power loss can’t cause it. With ctrl_power_pmic the board’s supply drops first, so 0.5 s is plenty; with IN2 alone keep it above the opto’s lag (~1.7 s on the bench, so 3000).',
  uart_console: 'Also run this console on the board’s serial pins (13 RX, 14 TX; 3.3 V, 250 kbaud) for a USB-to-UART adapter. Bench power testing only: the adapter keeps its port while the board is unpowered. Leave off at the install.',
  ctrl_settle_ms: 'House: after the controller powers up (or the house boots), its changes count as sync for at least this long. Raise it if the controller takes longer to settle after power returns.',
  fault_out: 'Use D5 as a “needs attention” output for the alarm system: high while this board is healthy (radio working, link up and, as the gate sees or reports it, AC power present and no limit fault), low otherwise, and low after a restart until the board has started up. A dead board or a cut wire reads as a fault too. 3.3 V, a few mA: wire it to an opto or a relay module input, never a relay coil. Off: D5 stays an unused input. Takes effect at once; set it on each board that has D5 wired.',
  fault_hold_s: 'How long a problem must last before D5 goes low, so a short dropout or blip doesn’t trip the alarm. Recovery raises D5 at once. 0 = low as soon as a problem appears.',
};
// Settings that must be identical on both boards (marked * in the form). The link's retry and response timing
// assumes the peer's frames use the same air settings, so cr counts even though the LoRa header carries it.
// tx_power only sets how loud each board transmits and may differ.
export const MUST_MATCH = new Set(['net_id', 'freq_hz', 'sf', 'bw_hz', 'cr', 'sync_word']);
export const SELECTS = {
  role: [[0, 'unset'], [1, 'house'], [2, 'gate']],
  bw_hz: [[125000, '125 kHz'], [250000, '250 kHz'], [500000, '500 kHz']],
};
