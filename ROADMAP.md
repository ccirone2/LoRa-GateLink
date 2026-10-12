# Roadmap

Desired features and ideas. Bugs and test tasks go in [TODO.md](TODO.md); shipped features are in the
[release notes](https://github.com/ccirone2/LoRa-GateLink/releases).

Items are grouped by area and numbered (F firmware, C web console, H hardware, B test bench, D development
environment). Each has a title, why it's wanted, and its cost, risks and open questions; a decision is recorded in
the item with its date. Ids are stable: refer to them in pull requests, TODO.md and the threat model, and never
reuse one; remove an item once it ships (the pull request and release notes describe it). Anything
touching gate behaviour must keep the behavioural invariants in [CLAUDE.md](CLAUDE.md). Hardware stays within about
$120 per location.

## Priority ranking

Most important first, by what each item does for the goals in CLAUDE.md (Alarm.com sees the gate's real state; robust,
hardened behaviour; the homeowner, installer and developer), then by effort within a tier. Effort: **S** days,
**M** a week or two, **L** several weeks or a MINOR change both boards need, **XL** the largest.

| Rank | Id | Item | Effort | Needs first |
|---|---|---|---|---|
| **Tier 1: the gate's real state, safety, never late** | | | | |
| 1 | F20 | Allow CLOSE without AC while a limit reads | S | |
| 2 | F9 | Reset breadcrumbs and reboot-loop detection | S | |
| 3 | F12 | Independent closed sensor | M | an input (H1, or gate IN4) |
| 4 | F14 | Plausibility checks at the gate | M | F9 for the reboot loop |
| 5 | F17 | Report a move the alarm missed | M | best with F6 (one frame change) |
| 6 | F6 | Encrypt payloads, close the withheld-command gap, harden the protocol | L | decide F8's addressing with it |
| **Tier 2: remote diagnosis, the installer's work, robust hardware** | | | | |
| 7 | D1 | Move to uv | S | |
| 8 | F10 | Firmware and settings mismatch check | S | |
| 9 | C3 | Live aiming mode for the site survey | S | |
| 10 | B4 | A cheap logic analyzer | S | |
| 11 | B1 | A reproducible RF bench | S | |
| 12 | F3 | Finer trouble reporting | M | F9 |
| 13 | C1 | Health verdict on the Status tab | M | F9, F10 for their lines |
| 14 | F1 | Persistent event journal | M | |
| 15 | F2 | Read the gate through the house | L | F1 for the log |
| 16 | H1 | A custom carrier board | L | settles F3, F4, F12's parts |
| 17 | F7 | Gate firmware update over LoRa | XL | F1, F2, F9 |
| **Tier 3: worthwhile, less urgent** | | | | |
| 18 | F4 | Opener health telemetry | S | a divider (H1 or by hand) |
| 19 | F19 | Tamper switch | S | an input (H1, or gate IN4) |
| 20 | B5 | More GateSim senses | S | |
| 21 | C2 | Journal timeline | M | F1, F2 |
| 22 | F11 | Hardware-specific code behind clean boundaries | M | |
| 23 | B2 | An adversary node on the RF bench | M | B1 |
| 24 | B3 | Fault hooks for bench builds | M | |
| 25 | D2 | Hardware-in-the-loop CI | M | |
| 26 | F18 | Console lock | M | |
| 27 | F16 | Detect a Shelly that reboots without losing power | M | |
| 28 | F5 | Radio changes with automatic revert | L | |
| 29 | F13 | Controllers that pulse instead of holding a switch | L | |
| 30 | F8 | More than one gate per house | L | the addressing decided with F6 |
| **Tier 4: later, or waiting on a decision** | | | | |
| 31 | C5 | Serve the console from its own origin | S | the owner's domain |
| 32 | F15 | Use the spare inputs | S | allocate inputs first (F12, F19, F14) |
| 33 | C4 | Verify the running image | M | |
| 34 | B6 | Replay field logs in the simulation | M | F1 |
| 35 | H2 | A different radio board: not now | XL | F11 |

## Firmware

### F1: Persistent event journal

The hourly link history persists in flash since 0.14.0, but log events live only in a 64-entry RAM ring stamped with
`millis()`, so a reset loses them, and a flood of replayed frames overwrites it
(`replay-flood-erases-evidence-and-reflects`).
- Write events to the unused flash sectors from 20 up (config 0–1, boot counter 2–3, history 4–19) with
  `histlog.cpp`'s record envelope; leave room for F7's staged image when allocating them.
- Stamp each event with the boot count and uptime; record wall-clock time when the web console connects, so the
  console can show real times.
- Keep the RAM ring in `.noinit` memory, so the last events before a watchdog reset are written at the next boot.
- Let `log.get` page older entries.
- Evidence of a replay flood: coalesce repeated flood events into one entry with a count, add replays to the history
  bucket (a `HLOG_FMT` bump), and limit how often a memo'd seq is re-ACKed.
- Cost / risk: each flash access stalls the loop ~0.5 s with the radio off the air, so batch the writes and never
  write during a pulse (as `histSave`). A PATCH. C2 shows it as a timeline.

### F2: Read the gate through the house

Today `remote.diag` returns only the gate's firmware version, uptime, six counters and its remote-writable settings.
Let the house page:
- the gate's log (from flash once F1 exists);
- its link history, and older buckets of either board's history from the flash log (it holds 512, three weeks;
  `hist.get` serves only the 96 a boot loads into RAM), read in batches since each read takes the radio off the
  air ~0.5 s. Or keep RAM slimmer buckets (68 B now) or 2 h ones: a `config.get` reply takes ~5 KB of heap;
- its full status: health, supply, AC, `loop_max_us` and `free_ram`.
- The gate answers only with no command pending and no relay pulsing; pages are rate-limited.
- Cost / risk: new message types, a MINOR both boards need together. Update the console contract and the threat model
  (payloads stay readable until F6). F9, F10 and C2 use this data.

### F3: Finer trouble reporting

`jamming-hides-state-until-link-timeout`. Since 0.14.0 D5 (`fault_out`) drives a second 2GIG sensor for radio,
link, AC, `no_power` and `fault` problems. Still missing:
- A travel timeout (jam). The house already gets `last_result` in STATUS; how long it stays latched needs deciding.
- A reboot loop (F9): a board that is alive but useless.
- A sensor per problem class, e.g. AC lost vs link lost.
- Whether the house controller's own power (IN2, the board's supply) counts.
- Hardware: an AQY212EH PhotoMOS (~$2) could replace the relay module, plus $25–35 per extra 2GIG sensor. It needs
  up to 3 mA (5–10 mA recommended) and the SAMD core leaves high drive strength off, so either set it on D5 with
  ~330 Ω or add a transistor. Part of H1.

### F4: Opener health telemetry

Warn before the opener's battery dies in a long AC outage, and spot wear, from inputs we have plus a divider.
Diagnostics only, like `history.cpp`: in STATUS and the history, never in a decision.
- The opener's 24 V accessory rail (battery-backed) through a voltage divider on a free analog pin (A0, A5 and A6
  are unused in `pins.h`; ~$0.50 in resistors): "opener battery low" before the gate goes `no_power`. Check how the
  buck shares ground with that rail first.
- The gate board's own LiPo and 5 V rail: a LiPo keeps it reporting through a cut; since 0.11.0 both boards read VIN
  power good from the charger chip (status `supply`), which the gate could put in STATUS. The chip has no ADC, so
  the LiPo voltage needs a divider too.
- Travel time per open and close (pulse or limit release to the other limit) and a cycle count: a gate slowing over
  weeks points at the battery, hinges or arm. Open question: have the web console suggest a `travel_timeout_s` from
  the record, rather than setting it by itself.

### F5: Radio changes with automatic revert

Both boards agree on a new SF, power or frequency over the link, switch at an agreed time, and go back to the old
settings if the link doesn't verify within N seconds.
- Why: radio parameters can't be written remotely today, because a bad change would strand the link. This lifts
  that limit, and enables a "raise SF when the margin is poor" helper.
- Cost / risk: a frame-format MINOR. It lifts the invariant that radio params are never remote-writable (decided
  2026-10-12 to keep it as written), so change CLAUDE.md, `docs/config.md`'s *remote* column, the threat model (a key
  holder or a buggy change can move the link, with the revert as the only backstop) and `docs/protocol.md` with it.
  The revert runs only after a failure, so host-test it exhaustively: a lost agreement frame, one board switching
  alone, a reset or power cut at each step, a revert that also fails.

### F6: Encrypt payloads, close the withheld-command gap, harden the protocol

One frame-format change, a MINOR both boards need together, for the open radio threats in the
[threat model](docs/threat-model.md). The boards refuse the old format rather than fall back.
- Encryption (`cleartext-traffic-analysis`): anyone listening learns the gate state and when mains power is lost,
  which is useful to a burglar. AES-CTR (the Crypto library has AES), encrypt-then-MAC, with per-direction keys
  derived per session from the challenge (`session-id-repeat`), so a CTR nonce of session and seq can never repeat
  under one key even if a session id does.
- Command freshness (`jam-and-delay-replay-window`, `withheld-command-fires-late`): a CMD jammed at the gate and
  recorded can still be delivered later inside the replay window, and encryption doesn't fix that (the frame still
  decrypts and verifies). Stamp each CMD with the gate's session clock, which the house estimates from STATUS, and
  have the gate refuse any CMD older than `cmd_ttl_s`.
- Challenges (`challenge-library-session-rebind`): a 128-bit HELLO challenge bound to the challenger's session, and
  never re-verifying a session superseded since boot.
- Decide F8's addressing and F17's STATUS field in the same change, so the wire format changes once before 1.0.

### F7: Gate firmware update over LoRa

The gate board is outdoors, so every update means a laptop at the gate box. The biggest saving in field effort.
- Stage the image in the SPI flash (~120 KB of the free sectors, allocated with F1).
- Require Ed25519-signed images (the Crypto library has it), with the private key off both boards. An unsigned or
  link-key-signed path would be a remote takeover route.
- The house sends, and only to a gate that has reported its version. Apply only when the gate is idle with AC on,
  never during a pulse; confirm or roll back after boot.
- Time: a ~120 KB image at SF9 / 500 kHz (~7 kbps raw) is ~2.5 minutes before protocol overhead, paced so the link
  keeps working.
- Cost / risk: the most work and the most risk here, since a failed update strands a board at the gate. Do F1, F2 and
  F9 first so a failed update can be diagnosed. It needs a bootloader stage that copies the staged image into place:
  check whether Arduino's SFU loader supports this board (not confirmed). Give it its own threat-model section, and
  host-test power cuts mid-copy.

### F8: More than one gate per house

E.g. a pedestrian gate beside the drive gate: the obvious growth case.
- Node addresses are fixed by role (house 1, gate 2) and STATUS carries one gate. One-to-many is a protocol change,
  so decide the addressing with F6, before 1.0, even if it's built later.
- Each gate needs its own controller input, contact sensor and fault output at the house, which the relay shield
  doesn't have (H1). Each extra gate end costs about $100.

### F9: Reset breadcrumbs and reboot-loop detection

`reboot-loops`. `reset_cause` says only "watchdog"; this says where the board hung, and flags a board that is alive
but useless. A prerequisite for F7.
- Use the SAMD21 watchdog's early-warning interrupt to save the program counter and the current loop stage in
  `.noinit` RAM; log them at the next boot (into the F1 journal once it exists). The handler only writes RAM.
- A reboot-loop counter: N resets within M minutes (or before the first full loop pass) raises the F3 reboot-loop
  trouble on D5.
- The gate reports its reboots and breadcrumbs to the house (in STATUS, or through F2).
- Cost / risk: small, firmware only.

### F10: Firmware and settings mismatch check

Today a mismatch shows up only as a link that never verifies or behaves oddly. Split in two (decided 2026-10-12),
since the must-match settings (`net_id`, `freq_hz`, `sf`, `bw_hz`, `cr`, `sync_word`) are exactly the ones that stop
the link when they differ, so a mismatched gate never answers over it:
- Over the link: the house compares the gate's firmware version (DIAG) with its own and warns, in status and the
  log, when the STATUS/DIAG layouts differ.
- In the web console: compare the must-match settings across both boards' configs (both connected in turn, or an
  exported file), and say "the link can't verify: these differ" next to *Peer verified: no*.
- Cost / risk: low; C1 shows it.

### F11: Hardware-specific code behind clean boundaries

Keep everything that touches the chip behind `board.h`, `radio` and `extflash`, so the rest of the firmware never
touches the hardware directly and a second board (H2) can sit beside the first rather than replace it. A refactor
with no behaviour change: the host tests pass unchanged.

### F12: Independent closed sensor

`closed-limit-nc-fail-unsafe` (High). "Closed" rests on one contact, the AUX2 relay's NC, which reads closed if that
relay fails with its 24 V present.
- A magnetic or reed sensor on the leaf (a few dollars) as a second closed input: the gate reports closed only when
  both agree, and a lasting disagreement reads `fault`, which the house already shows as not-closed.
- Needs an input (gate IN4 is contended, F15; or H1's extra inputs) and a host-test monitor that closed never shows
  with either sensor open.

### F13: Controllers that pulse instead of holding a switch

Many smart relays and garage controllers give a momentary contact rather than an on/off level; supporting them would
let the house work with far more than the Shelly (the web console already calls it the generic "controller").
- Needs a design for how a pulse maps to OPEN/CLOSE (a toggle against the gate's state is ambiguous mid-travel), how
  K1 sync works without a level to mirror, and how it keeps the power-sense and sync-window rules.
- One candidate to bench-test (decided 2026-10-12: here rather than its own item): a native Z-Wave garage
  controller such as the Ecolink GDZW7 or Linear GD00Z, which Alarm.com shows as a real open/close device with state
  and which might replace the K1-mirror and sync machinery. Questions: how it takes state (its own wireless tilt
  sensor, which the house would have to drive), its UL 325 warn-before-move (a beep and delay before closing), and
  dealers blocking devices they didn't sell.

### F14: Plausibility checks at the gate

`closed-limit-nc-fail-unsafe`, `power-sense-degradation-silent`, `relay-held-welded-or-overlapping`. Latch faults the
inputs alone don't show: a closed limit that never releases after our OPEN pulse, closed reached without passing
between, IN2 or IN3 off while everything else says power is there, a shared input held on while our coil is off
(that last needs the opener's OPEN/CLOSE terminals sensed on an input: F15, or H1). Reboot loops are F9.

### F15: Use the spare inputs

Gate IN4 and house IN3/IN4 are wired, debounced, logged and reported but have no behaviour. Candidates: a beam-break
or vehicle-presence sensor at the gate reported to the house (and on to Alarm.com), or the alarm panel's armed state
at the house. Needs a decision on what Alarm.com should see and whether any of it may affect commands (the opener's
own safety inputs stay in charge of entrapment). Gate IN4 is the gate's only spare, and F12, F14 and F19 want it
too: allocate it, or add inputs (H1).

### F16: Detect a Shelly that reboots without losing power

`shelly-reboot-spurious-commands`. IN2 only senses the Shelly's supply; an internal reboot can still produce an IN1
edge. Ideas: watch for the Shelly's relay dropping and coming back within a short window with no matching Alarm.com
change, or sense its status LED.

### F17: Report a move the alarm missed

`link-loss-concealment-window`. A "left closed since you last heard" latch or movement counter in STATUS: when the
house sees it advance while it showed closed (also on link recovery), it opens K2 for a few seconds (and D5) so the
alarm records the opening late rather than never. A STATUS change: ship it with F6.

### F18: Console lock

`no-console-authentication-root`. An install-time setting after which state-changing console commands (relay.test,
config.*, key.set, remote.*, debug.*) need proof of the key, leaving status, logs and history open. Reflashing still
bypasses it; it stops casual USB actuation at the gate box.

### F19: Tamper switch

`key-extract-physical`, `malicious-firmware-reflash`. An enclosure switch on the gate (IN4, or an H1 input) reported
in STATUS, shown by the house as needs-attention and logged, so box access is noticed and the key rotated.

### F20: Allow CLOSE without AC while a limit reads

`ac-cut-refuses-close`; decided 2026-10-12. Today the gate refuses every command while IN3 reads no AC
(`RES_NO_POWER`, `role_gate.cpp`), because the opener may be on its last battery reserve and the AES controller
sharing its inputs is unpowered. So cutting AC keeps an open gate open, and Alarm.com can't close it. CLOSE is the
secure direction:
- CLOSE runs as usual when a limit reads: it pulses from open (or to reverse an OPEN), and is `already` at closed.
- OPEN stays refused without AC, and with no limit reading (`no_power`) both stay refused.
- A battery too weak to finish the travel leaves the gate `no_power`, which the house already shows as not-closed.
- No setting to keep the old behaviour (decided 2026-10-12): one behaviour, fewer knobs.
- Gate only (the house handles the ACK as any other), so a PATCH. Update with it the AC-power invariant in CLAUDE.md,
  the README's "How it behaves", `docs/hardware.md` (IN3), the `power_sense` help in `web/js/settings.js`, and the
  threat model. Host tests: CLOSE without AC from open pulses, OPEN refuses, `no_power` refuses both, and the house's
  resync after a refused OPEN still happens. Bench: the power tests.

## Web console

### C1: Health verdict on the Status tab

One plain-language line with a next step, e.g. "All good", "Link weak: 6 dB margin, raise SF on both boards", "Gate
on battery: AC lost 2 h ago". The current counters move under *Details*. It also shows F10 mismatches and F9 reboot
loops. For the installer first: the homeowner never sees the console.

### C2: Journal timeline

The F1 journal as a readable timeline, e.g. "14:02 Opened from the controller, open in 14 s", "03:10 Moved locally
(keypad or other controller)", readable remotely through the house via F2. Answers "what happened last night?"
without a laptop at the gate. (The naming rule applies: no brand names in `web/`.)

### C3: Live aiming mode for the site survey

The survey card and `tools/gatelink.py survey` measure margin, but only as a verdict after a timed run. Add an
open-ended *Aim* mode that pings every 2 s until stopped and shows the latest margin above the SF floor in both
directions in large type, coloured by the survey's good/fair thresholds (the same `survey.js` / `survey.py` logic and
shared vectors), and a matching `survey --live`. The installer walks the antenna to its best spot, then runs the timed
survey for the install record.

### C4: Verify the running image

`tamper-undetectable`. Flashing already checks what it wrote (the web console's SAM-BA CRC, bossac's verify); this is
a check of a board at any later visit: read the application back over the bootloader and compare it with the
release's image, from the tools and the web console.

### C5: Serve the console from its own origin

`shared-github-io-origin`. A custom domain or a separate organisation, so other Pages sites of the account can't use
its Web Serial grants. Needs the owner's domain or account. Deferred (2026-10-12): maybe eventually.

## Hardware

At list price the MKR WAN 1310 (~€48) and the Relay Proto Shield (~€21) already come to about $75–80 per end, before
the opto board, buck, antenna, LiPo and enclosure (about $110–125 in all). Upgrades need to replace parts, not add
them.

### H1: A custom carrier board

The hardware change most worth making. A KiCad board, assembled by JLCPCB, with the MKR socketed so the firmware is
unchanged. It replaces the relay shield, the opto board, the buck and the proto wiring, and removes hand wiring as a
fault source (and most of the polarity and voltage warnings in `docs/hardware.md`). It carries:
- opto inputs with the PNP-equivalent polarity the pull-down inputs need, TVS on the field side, and more inputs than
  IN1–IN4 (F12, F14, F15, F19 contend for them);
- a wide-input buck (rated to 36 V: the 24 V rail runs higher while the opener's battery charges) and a bulk
  capacitor;
- the relays, the F3 PhotoMOS output, the F4 voltage divider and screw terminals labelled per role.
- Off the board, at the gate box: a gasketed enclosure with cable glands and a grounded bulkhead for the antenna.
- Cost: roughly $15–25 per board in quantities of 5–10, against ~$35 for the parts it replaces. Keep the footprint
  reusable if the board changes (H2).

### H2: A different radio board: not now

An STM32WL or SX1262 board (e.g. ESP32-S3 + SX1262, ~$20) would roughly halve the board cost.
- The SX1262 brings lower receive current (~4.6 mA against ~10 mA) and +22 dBm output; its sensitivity at matched SF
  and bandwidth is about the same as the SX1276's.
- The ESP32-S3's secure boot and flash encryption would stop physical access from yielding the key
  (`key-extract-physical`, `malicious-firmware-reflash`, accepted today because the SAMD21 can't); Wi-Fi stays off.
- Cost: `radio.cpp` encodes a lot of hard-won SX1276 behaviour; a new radio driver, a flash and config backend,
  esptool-js in place of SAM-BA in the web console, and the bench numbers measured again. Only worth it for many
  sites; F11 lets it sit beside the MKR.

## Test bench

### B1: A reproducible RF bench

Fixed SMA attenuators and a shielded box (a metal tin works; about $30, bench equipment outside the per-location
budget) give a known, repeatable link margin, so `-m rf` stops depending on unplugged antennas. Covers the bench half
of "Real RF" in TODO.md and gives `rftest` a calibrated pass line.

### B2: An adversary node on the RF bench

The rejected board (…191117, TODO.md / `docs/bench-testing.md`) as a third node inside B1's attenuated box: replay
frames, send foreign traffic on our SF and sync word, keep the channel busy, and withhold and late-deliver a CMD to
verify F6's freshness check. That exercises the listen-before-talk and replay paths over real RF, not only in the
host simulation. Run it as a separate bench-only sketch, like the GateSim, never a mode in GateLink firmware
(`debug-commands-in-production`), and only at low power inside the box.

### B3: Fault hooks for bench builds

A build flag compiling in fault injection (reset the radio mid-pulse, time a controller edge, replay CFG_SETs) for the
0.13.9 paths only the host tests reach (TODO.md); the release workflow checks it is off in release binaries. B2 covers
the over-the-air ones.

### B4: A cheap logic analyzer

A ~$15 USB analyzer with sigrok/PulseView, enough to check D5 (and the K1/K2 outputs) for glitches through resets and
boots (the open D5 item in TODO.md). Without one, a brief pulse there can't be seen.

### B5: More GateSim senses

Current sensing (an INA219, ~$5) on each board's 5 V feed to measure the dips behind the full-power TX resets; D5 and
house K1 wired to GateSim inputs so the suite checks them (TODO.md lists both gaps).

### B6: Replay field logs in the simulation

Load a board's exported log (the F1 journal) and link history into the host simulation, so a problem seen at the
site becomes a test instead of a bench reconstruction.

## Development environment

### D1: Move to uv

A `pyproject.toml` and `uv.lock` for `tests/e2e`, `tests/tools` and `tools/`, used as `uv run pytest` and
`uv run tools/gatelink.py`, with `astral-sh/setup-uv` in CI, in place of `requirements.txt` and pip (today lower bounds
only). The lock carries hashes, which closes `python-deps-unpinned` (TODO.md, bench hygiene). Update
`docs/development.md`, the skills and `tools/agent/cloud_setup.sh` with it.

### D2: Hardware-in-the-loop CI

A self-hosted runner on the bench PC running a smoke subset (`/flash`, preflight and `test_normal`), reporting
`summary.json`. The repository is public, so it runs only on `workflow_dispatch` and pushes to `main` (decided
2026-10-12): never on `pull_request` or a label, which would run a fork's code on the bench PC beside the key and the
Home Assistant token. A branch of the repository itself can be run by dispatching on it. Needs a port lock so it never
runs while someone has the web console open.
