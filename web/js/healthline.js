// The one-line health verdict above every tab (ROADMAP C1): the most important thing wrong with the site
// as the connected board sees it, and what to do next. Most important first, by the project's goals: what keeps the
// alarm from seeing the gate's real state or the gate from moving, then what to diagnose. Pure, so it is unit-tested
// under Node: healthLine(status, peer) with peer = peerDiffs() (peer.js).
import { FAIR_DB, sampleMargin, fmt1 } from './survey.js';

const STARTUP_MS = 30000;
const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
const startingLine = (s) => ({ level: 'info',
  text: s.role === 'house' ? 'Starting up: waiting for the gate’s first report and the controller to settle.'
    : 'Starting up: the gate inputs are settling.' });
const words = (t) => String(t).replaceAll('_', ' ');

// The weaker direction's margin above the SF's floor from the latest frames (null when unknown): one frame each way,
// so a hint to survey, not a verdict on the site.
export function linkMargin(s, sf) {
  if (!isNum(sf)) return null;
  const l = s.link || {};
  const here = l.age_ms >= 0 ? sampleMargin(sf, l.snr, l.rssi, l.noise) : null;
  const r = s.role === 'house' ? s.remote || {} : {};
  const there = r.uptime_s ? sampleMargin(sf, r.snr, r.rssi, r.noise) : null;
  const m = [here, there].filter(isNum);
  return m.length ? Math.min(...m) : null;
}

// { level: 'good' | 'info' | 'warn' | 'bad', text } for status reply `s`, with margin the link margin to judge (dB, e.g.
// linkMargin() over several polls; null: unknown) and peer what peerDiffs() says about the other board
// ({ other, there, diffs, fwMinor }).
export function healthLine(s, { margin = null, peer = null } = {}) {
  const health = Array.isArray(s.health) ? s.health : [];
  const other = s.role === 'house' ? 'gate' : 'house';
  const line = (level, text) => ({ level, text });
  // Setup comes first; a missing key is said with it, as nothing else on the page says it while these show.
  const andKey = s.key_set === false ? ' The board has no link key either: write one under Board.' : '';
  if (s.reboot_pending) return line('warn', `A saved role waits for a reboot: Config → Reboot.${andKey}`);
  if (s.role !== 'house' && s.role !== 'gate') return line('info', `Role not set: choose house or gate under Config, save, then reboot.${andKey}`);
  if (s.key_set === false) return line('bad', 'No link key: write the same key to both boards under Board.');
  if (s.radio_ok === false) return line('bad', 'The radio isn’t working: reboot the board; if it stays down, the board needs checking.');

  // `reboots` on the house is its own loop or the gate's (STATUS bit 7): its own has the crash resets to show.
  if (health.includes('reboots') && s.crash_resets >= 3) {
    const where = s.hang_stage ? `, last hung in ${words(s.hang_stage)}` : '';
    return line('bad', `This board keeps rebooting (${s.crash_resets} crash resets in a row${where}): check its supply, then report it.`);
  }
  if (s.role === 'house' && s.remote?.reboot_loop) {
    return line('bad', 'The gate keeps rebooting: check its supply; connect to the gate board for where it hangs.');
  }

  // Just after boot the link isn't up yet either: say so for the first half minute, then call it a missing link.
  const starting = health.includes('starting');
  if (starting && s.uptime_ms < STARTUP_MS) return startingLine(s);
  const linkDown = s.role === 'house' ? s.link_up === false : health.includes('link') || s.link?.verified === false;
  if (linkDown) {
    const diffs = peer?.diffs ?? [];
    if (diffs.length) {
      const what = diffs.map((d) => d.name).join(', ');
      return line('bad', `The link can’t verify: ${what} differ${diffs.length === 1 ? 's' : ''} from the ${other} (see Other board). Make them match.`);
    }
    if (peer?.fwMinor) {
      return line('bad', `No link to the ${other}, and its firmware differs (${s.fw} here, ${peer.there.fw} there): update both boards.`);
    }
    return line('bad', `No link to the ${other}: check it has power and its antenna, and that both boards have the same key and radio settings; then Link → Ping.`);
  }

  if (s.gate === 'no_power') return line('bad', 'The gate opener has no power and no limit reads: check the opener and its battery. The alarm shows the gate not closed meanwhile.');
  if (s.gate === 'fault') return line('bad', 'Both gate limits read at once, or have since the open limit last read alone: check the limit wiring and the opener’s AUX relay settings. Once fixed, a full opening (or a gate board restart) clears it. The alarm shows the gate not closed meanwhile.');
  if (health.includes('ac')) return line('warn', 'The gate is on its battery: AC power lost. Commands still work while a limit reads; check the opener’s mains supply.');
  if (s.role === 'house' && s.ctrl_power === false) return line('warn', 'The controller has no power: its switch is ignored until power returns.');
  if (starting) return startingLine(s);

  if (peer?.fwMinor) return line('warn', `Firmware differs from the ${other} (${s.fw} here, ${peer.there.fw} there): update both.`);
  if (peer?.diffs?.length) return line('warn', `Settings differ from the ${other} as last seen (${peer.diffs.map((d) => d.name).join(', ')}): check Other board.`);
  if (isNum(margin) && margin < FAIR_DB) {
    return line('warn', `Link weak: ${fmt1(margin)} dB above the floor on recent frames. Run Link → Site survey; raise sf on both boards if it stays under ${FAIR_DB} dB.`);
  }
  if (s.supply === false) return line('warn', 'The board is running on its LiPo: its supply (VIN) is off.');
  if (s.hang_stage) return line('warn', `The last reset was the watchdog: the board hung in ${words(s.hang_stage)}. Report it with the log.`);
  if (health.length) return line('warn', `Needs attention: ${health.map(words).join(', ')}.`);
  return line('good', `All good: link up, gate ${words(s.gate || 'unknown')}.`);
}
