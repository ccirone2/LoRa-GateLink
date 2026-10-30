# Roadmap

Desired features and ideas. Bugs and test tasks go in [TODO.md](TODO.md); shipped features are in the
[release notes](https://github.com/ccirone2/LoRa-GateLink/releases).

Items are grouped by area and numbered (F firmware, C web console, H hardware, B test bench, D development
environment). Each one leads with its goal: the title names the outcome, **Goal** says what it achieves and for whom,
and **Today** says what falls short now. The method comes after: **How** (a proposal, open to a better way of reaching
the goal), **Cost / risk** and **Open questions**. A decision is recorded in the item with its date. Ids are stable:
refer to them in pull requests, TODO.md and the threat model, and never reuse one; remove an item once it ships (the
pull request and release notes describe it). Anything touching gate behaviour must keep the behavioural invariants in
[CLAUDE.md](CLAUDE.md). Hardware stays within about $120 per location.

> **Treat every item with cautious skepticism.** These are ideas, not vetted plans: not every item has been checked
> against the code, the hardware or the threat model, and the figures in them (prices, timings, currents) are
> estimates. An item's consequences can outweigh its benefit. Before building one, start with its cost and risks:
> read its **Cost / risk**, check the claims it rests on, work out what it could break (the behavioural invariants,
> the threat model, both boards in the field, the installer's work), and ask whether a smaller change reaches the
> same goal. If the risk isn't worth it, say so and record why in the item, or remove it.
> `/implement` builds items this way.

## Priority ranking

Most important first, by what each item does for the [goals](README.md#goals) (in order: truth, availability, safety,
diagnosability; then the homeowner, installer and developer, in that order), then by effort within a tier. Effort:
**S** days, **M** a week or two, **L** several weeks or a MINOR change both boards need, **XL** the largest.

| Rank | Id | Item | Effort | Needs first |
|---|---|---|---|---|
| **Tier 2: remote diagnosis, the installer's work, robust hardware** | | | | |
| 1 | F3 | Tell the alarm what kind of trouble it is | M | |
| 2 | F2 | See the gate's log, history and status from the house | L | |
| 3 | H1 | Cheaper ends that are hard to wire wrong | L | settles F3, F4's parts |
| 4 | F7 | Update the gate's firmware without visiting it | XL | F2 |
| **Tier 3: worthwhile, less urgent** | | | | |
| 5 | F4 | Warn before the opener's battery or mechanics fail | S | a divider (H1 or by hand) |
| 6 | F14 | Catch faults the inputs alone don't show | S | an input (H1) |
| 7 | F19 | Notice when a box is opened | S | an input (H1, or gate IN4 where no closed sensor is fitted) |
| 8 | B5 | Let the suite check D5, K1 and supply dips | S | |
| 9 | C2 | Answer "what happened last night?" | M | F2 |
| 10 | F11 | Firmware ready for a second board | M | |
| 11 | B2 | Test attacks over real RF | M | a shielded box |
| 12 | B3 | Reach the host-only fault paths on the bench | M | |
| 13 | D2 | Run the bench automatically, safely | M | |
| 14 | F18 | Stop casual actuation over USB | M | |
| 15 | F16 | Ignore a Shelly that reboots without losing power | M | |
| 16 | F5 | Change radio settings without stranding the link | L | |
| 17 | F13 | Work with controllers that pulse | L | |
| **Tier 4: later, or waiting on a decision** | | | | |
| 18 | F8 | Serve more than one gate from one house | L | H1, and a second gate at this site |
| 19 | C5 | Keep other sites from using the console's port grants | S | the owner's domain |
| 20 | F15 | Put the spare inputs to use | S | allocate inputs first (F19, F14) |
| 21 | B6 | Turn field incidents into tests | M | |
| 22 | H2 | Cheaper boards that protect the key: not now | XL | F11 |

## Firmware

### F2: See the gate's log, history and status from the house

**Goal.** Diagnose the gate end without a trip to the gate box: from the house board's USB, the installer and
developer see what the gate has seen.

**Today.** `remote.diag` returns only the gate's firmware version, uptime, six counters and its remote-writable
settings, and `hist.get` serves only the 96 hourly buckets a boot loads into RAM, though the flash log holds 512
(three weeks).

**How.** Let the house page:
- the gate's log, its flash journal included (`log.get` with `before`, since 0.19.0);
- its link history, and older buckets of either board's history from the flash log, read in batches since each read
  takes the radio off the air ~0.5 s (or keep slimmer buckets in RAM, 68 B now, or 2 h ones: a `config.get` reply
  takes ~5 KB of heap);
- its full status: health, supply, AC, `loop_max_us`, `free_ram`, and `crash_resets` with the last hang (`hang_stage`,
  `hang_pc`; since 0.15.0 the house learns only the gate's reboot loop, STATUS bit 7).

The gate answers only with no command pending and no relay pulsing, and pages are rate-limited.

**Cost / risk.** New message types: a MINOR both boards need together. Update the console contract and the threat
model (payloads are encrypted since 0.16.0). C2 uses this data.

### F3: Tell the alarm what kind of trouble it is

**Goal.** The homeowner's alarm app says which problem the gate has (stuck, AC lost, link lost, a board stuck
rebooting), not only that it needs attention, and covers every problem that leaves the gate unattended
(`jamming-hides-state-until-link-timeout`).

**Today.** Since 0.14.0 D5 (`fault_out`) drives one second 2GIG sensor for radio, link, AC, `no_power` and `fault`
problems, since 0.15.0 a reboot loop at either board (health `reboots`), and since 0.17.3 a dead house IN2 opto
(`ctrl_sense`), and since 0.18.0 a gate stopped between its limits for longer than its `travel_timeout_s` (`stuck`),
all alike.

**How.**
- A sensor per problem class, e.g. AC lost vs link lost.
- Hardware: an AQY212EH PhotoMOS (~$2) could replace the relay module, plus $25–35 per extra 2GIG sensor. It needs
  up to 3 mA (5–10 mA recommended) and the SAMD core leaves high drive strength off, so either set it on D5 with
  ~330 Ω or add a transistor. Part of H1.

**Open questions.** How long a jam stays latched; how many sensors are worth their cost. Settled 2026-10-17 (F14): the
controller losing its power isn't a problem of its own (Alarm.com already shows the Z-Wave device unreachable), but IN2
disagreeing with the board's supply is (`ctrl_sense`).

Decided 2026-10-19 (`/implement`, verdict *build smaller*): the jam part built as health `stuck` (0.18.0), the gate
`between` for longer than `travel_timeout_s`, on each board from its own view (no STATUS change), cleared at once by a
limit: it needs no latch, and it catches a gate stopped by anyone, not only after our command. Not built: a trouble
for our command timing out with the gate still at its limit (the opener locked out, a photo-eye blocked, another
controller holding an input). Telling that from an override to the other limit needs a new STATUS result (both boards
together), the alarm switch already flips back to the real state when it happens, and a blocked photo-eye would make
it a nuisance until the next command succeeds.

### F4: Warn before the opener's battery or mechanics fail

**Goal.** The homeowner hears that the opener is on a failing battery, or wearing out, before the gate stops working,
especially through a long AC outage.

**Today.** The gate learns the opener's battery is flat only when the limits drop (`no_power`), and nothing tracks
how the gate's travel changes over time.

**How.** Diagnostics only, like `history.cpp`: in STATUS and the history, never in a decision.
- The opener's 24 V accessory rail (battery-backed) through a voltage divider on a free analog pin (A0, A5 and A6
  are unused in `pins.h`; ~$0.50 in resistors), for an "opener battery low" warning. Check how the buck shares
  ground with that rail first.
- The gate board's own LiPo and 5 V rail: since 0.11.0 both boards read VIN power good from the charger chip (status
  `supply`), which the gate could put in STATUS; the chip has no ADC, so the LiPo voltage needs a divider too.
- Travel time per open and close (pulse or limit release to the other limit) and a cycle count: a gate slowing over
  weeks points at the battery, hinges or arm.

**Open questions.** Should the web console suggest a `travel_timeout_s` from the record, rather than the board
setting it by itself?

### F5: Change radio settings without stranding the link

**Goal.** An installed pair can move to a better SF, power or frequency from the house, without a visit to the gate,
and a bad choice can never leave the link down for good. It also allows a "raise SF when the margin is poor" helper.

**Today.** Radio parameters can't be written over LoRa, because a bad change would strand the link; changing them
means reaching both boards.

**How.** Both boards agree on the new settings over the link, switch at an agreed time, and go back to the old ones if
the link doesn't verify within N seconds.

**Cost / risk.** A frame-format MINOR. It lifts the invariant that radio params are never remote-writable (decided
2026-10-12 to keep the item as written), so change CLAUDE.md, `docs/config.md`'s *remote* column, the threat model (a
key holder or a buggy change can move the link, with the revert as the only backstop) and `docs/protocol.md` with it.
The revert runs only after a failure, so host-test it exhaustively: a lost agreement frame, one board switching alone,
a reset or power cut at each step, a revert that also fails.

### F7: Update the gate's firmware without visiting it

**Goal.** A new firmware reaches the gate board from the house, safely, so the outdoor end never needs a laptop at the
gate box for an update. The biggest saving in field effort.

**Today.** Every update means opening the gate box and connecting over USB.

**How.**
- Stage the image in the SPI flash (~120 KB of sectors 36 up, left free for it).
- Accept only Ed25519-signed images (the Crypto library has it), with the private key off both boards: an unsigned or
  link-key-signed path would be a remote takeover route.
- The house sends, and only to a gate that has reported its version. Apply only when the gate is idle with AC on,
  never during a pulse; confirm or roll back after boot.
- Pace it so the link keeps working: a ~120 KB image at SF9 / 500 kHz (~7 kbps raw) is ~2.5 minutes before protocol
  overhead.

**Cost / risk.** The most work and the most risk here: a failed update strands a board at the gate. Do F2 first (the
event journal is in since 0.19.0) so a failed update can be diagnosed. It needs a bootloader stage that copies the staged image into place. Give
it its own threat-model section, and host-test power cuts mid-copy.

**Open questions.** Does Arduino's SFU loader support this board (not confirmed)?

### F8: Serve more than one gate from one house

**Goal.** One house end controls and reports several gates, e.g. a pedestrian gate beside the drive gate: the obvious
growth case.

**Today.** Node addresses are fixed by role (house 1, gate 2, `myNodeId`/`peerNodeId`), and the house keeps one
peer's link state and one gate's role state.

**How.**
- Give each gate its own node id (a setting) and keep the house's link state per peer: session, replay window,
  challenge, ACK memo, reliable slots and the gate clock its commands are dated in (0.16.0).
- Keep the role state per gate as well, and give each gate its own controller input, contact sensor and fault output
  at the house, which the relay shield doesn't have (H1).
- Give each gate its own key, held by the house beside the others, so a gate box opened exposes only that gate's link
  (a config record change: the record must still fit its 256-byte page).

**Cost / risk.** Firmware and hardware, not a frame-format change: a house that serves several gates still talks to a
single one as before. Each extra gate end costs about $110–125 (see Hardware). Build it only when this site has a
second gate (README "Goals": this site first).

**Decided 2026-10-13 (`/implement`):** the addressing needs no wire change, so it no longer waits on F6. The frame
header's `src` and `dst` bytes already address up to 255 nodes, and each gate sends its own STATUS frame, so STATUS
needs no gate index. F6 (0.16.0) keeps the per-node separation by construction (its CTR nonce includes `src` and `dst`, and
the session permutation already includes the node id). It waits on H1 and on a real second gate, so it moves to
tier 4.

### F11: Firmware ready for a second board

**Goal.** A second hardware target (H2) can be added beside the MKR rather than replacing it.

**Today.** Most chip-specific code already sits behind `board.h` (the host simulation builds everything from `app.cpp`
down except the radio, flash, charger and console drivers), but nothing keeps it that way.

**How.** Keep everything that touches the chip behind `board.h`, `radio` and `extflash`, so the rest of the firmware
never touches the hardware directly.

**Cost / risk.** A refactor with no behaviour change: the host tests pass unchanged.

### F13: Work with controllers that pulse

**Goal.** The house works with the many smart relays and garage controllers that give a momentary contact rather
than an on/off level, so GateLink isn't tied to the Shelly (the web console already calls it the generic
"controller").

**Today.** The house needs a controller whose output holds a level, and mirrors the gate onto its switch input (K1).

**How.** Design how a pulse maps to OPEN/CLOSE, how K1 sync works without a level to mirror, and how it keeps the
power-sense and sync-window rules. One candidate to bench-test (decided 2026-10-12: here rather than its own item): a
native Z-Wave garage controller such as the Ecolink GDZW7 or Linear GD00Z, which Alarm.com shows as a real open/close
device with state, and which might replace the K1-mirror and sync machinery.

**Open questions.** A toggle against the gate's state is ambiguous mid-travel. For the Z-Wave candidate: how it takes
state (its own wireless tilt sensor, which the house would have to drive), its UL 325 warn-before-move (a beep and
delay before closing), and dealers blocking devices they didn't sell.

### F14: Catch faults the inputs alone don't show

**Goal.** A failing power sense or relay contact is reported as a fault instead of quietly disabling remote control
(`power-sense-degradation-silent`, `relay-held-welded-or-overlapping`).

**Today.** Since 0.17.1 both limits reading at once holds the gate at `fault` until the open limit reads alone, so a
stuck closed limit (`closed-limit-nc-fail-unsafe`) reads not closed from the gate's first full opening on, and since
0.17.2 closed straight from open reads `fault`. A dead gate IN3 isn't silent (it reads as AC lost: health `ac`, D5).
Since 0.17.3 a dead house IN2 opto (IN2 off 30 s with the board's supply good) is health `ctrl_sense` (D5). A welded
relay contact is invisible.

**How.**
- A shared input held on while our coil is off: needs the opener's OPEN/CLOSE terminals sensed on an input (H1: the
  gate has no spare one since F12).

Decided 2026-10-15 (`/implement`, verdict *build smaller*): built the both-limits hold (0.17.1); not built, and not
to be: a fault when the closed limit stays on after our OPEN pulse (a gate that doesn't move, the opener locked out or
another controller holding CLOSE, is far likelier than a stuck limit with the gate stopped short of open, so it would
mostly show a closed gate as not closed; the travel timeout already reports it, `last_result` `timeout`); a gate IN3 check (nothing at the gate says AC is there apart from IN3, and
a dead IN3 already alarms). Moved from rank 2 to 5: what is left serves diagnosis, not the gate's real state.
Reversed 2026-10-16 (#139, 0.17.2): closed reached straight from open now reads `fault` after all, as the opener
restarting is a path to it. Built 2026-10-17 (0.17.3): the house IN2 check, as health `ctrl_sense`. Built 2026-10-17 (0.17.4), smaller: the
both-limits hold kept through a crash reset in the reset memo, not in the flash (a deliberate restart still clears it,
the installer's way after a repair).
Decided 2026-10-23 (`/implement`, verdict *rewrite the item*): the power-on hold dropped. With the closed sensor on
(`closed_sense`, F12, 0.20.0) a stuck closed limit with the gate part-open reads `between`, then `fault`, never
closed, after any restart; without it, a flash record for a power cut on top of a stuck limit isn't worth its format.
What is left, a welded contact on the shared opener inputs, needs those inputs sensed, and the gate's four are all in
use (IN4 went to the closed sensor), so it waits on H1. Moved from rank 3 to 7: diagnosis only, and blocked on hardware.

### F15: Put the spare inputs to use

**Goal.** The spare inputs carry something worth knowing, e.g. a vehicle at the gate, or the alarm's armed state at
the house.

**Today.** House IN3/IN4 are wired, debounced, logged and reported but have no behaviour; so is gate IN4 where no
closed sensor is fitted.

**How.** Candidates: a beam-break or vehicle-presence sensor at the gate reported to the house (and on to Alarm.com),
or the alarm panel's armed state at the house. Gate IN4 went to the second closed sensor (F12, 0.20.0, decided 2026-10-23: it serves truth, the
others diagnosis), so F14 and F19 need H1's inputs, or IN4 at a site without that sensor.

**Open questions.** What should Alarm.com see, and may any of it affect commands? (The opener's own safety inputs stay
in charge of entrapment.)

### F16: Ignore a Shelly that reboots without losing power

**Goal.** An internal Shelly reboot never moves the gate (`shelly-reboot-spurious-commands`).

**Today.** IN2 only senses the Shelly's supply, so a reboot with power present can still produce an IN1 edge, a CLOSE
then an OPEN.

**How.** Ideas: watch for the Shelly's relay dropping and coming back within a short window with no matching Alarm.com
change, or sense its status LED.

### F18: Stop casual actuation over USB

**Goal.** Someone at an unlocked box can't pulse the gate or change its safety settings or key just by plugging in
(`no-console-authentication-root`).

**Today.** The console has no authentication.

**How.** An install-time setting after which state-changing console commands (relay.test, config.*, key.set,
remote.*, debug.*) need proof of the key, leaving status, logs and history open.

**Cost / risk.** Reflashing still bypasses it; it stops casual use, not a determined attacker.

### F19: Notice when a box is opened

**Goal.** Access to a box is noticed, so the key can be rotated (`key-extract-physical`,
`malicious-firmware-reflash`).

**Today.** Physical access is invisible until someone looks.

**How.** An enclosure switch at the gate (an H1 input, or IN4 where no closed sensor is fitted), reported in STATUS, shown by the house as
needs-attention and logged.

## Web console

### C2: Answer "what happened last night?"

**Goal.** The installer or developer reads the site's recent events as a story, from the house, without a laptop at
the gate.

**Today.** The log is a list of coded events from one board, stamped with its uptime and boot number; since 0.19.0
it is kept across resets in a flash journal (`log.get` with `before`), but nothing dates it.

**How.** The event journal as a readable timeline, dated by the time the web console records when it connects (the
event log left wall-clock time out: uptime and boot number order events, but don't date them), e.g. "14:02 Opened from the controller, open in 14 s", "03:10 Moved
locally (keypad or other controller)", readable remotely through the house via F2. (The naming rule applies: no brand
names in `web/`.)

### C5: Keep other sites from using the console's port grants

**Goal.** Only the GateLink console can use the Web Serial grants a browser gives it (`shared-github-io-origin`).

**Today.** The console shares `ccirone2.github.io`, and its grants, with the account's other Pages sites.

**How.** Serve it from its own origin: a custom domain or a separate organisation. Needs the owner's domain or account.
Deferred (2026-10-12): maybe eventually.

## Hardware

At list price the MKR WAN 1310 (~€48) and the Relay Proto Shield (~€21) already come to about $75–80 per end, before
the opto board, buck, antenna, LiPo and enclosure (about $110–125 in all). Upgrades need to replace parts, not add
them.

### H1: Cheaper ends that are hard to wire wrong

**Goal.** Each end costs less, survives its field wiring, and can't be wired with the wrong polarity or voltage; it
also gives the inputs and outputs F3, F4, F8, F14, F15 and F19 need. The hardware change most worth making.

**Today.** A relay shield, an opto board, a buck and hand wiring, with no surge protection, four contended inputs,
and the polarity and voltage warnings that fill `docs/hardware.md`.

**How.** A KiCad carrier board, assembled by JLCPCB, with the MKR socketed so the firmware is unchanged. It carries:
- opto inputs with the PNP-equivalent polarity the pull-down inputs need, TVS on the field side, and more inputs than
  IN1–IN4;
- a wide-input buck (rated to 36 V: the 24 V rail runs higher while the opener's battery charges) and a bulk
  capacitor;
- the relays, the F3 PhotoMOS output, the F4 voltage divider and screw terminals labelled per role.
- Off the board, at the gate box: a gasketed enclosure with cable glands and a grounded bulkhead for the antenna.

**Cost / risk.** Roughly $15–25 per board in quantities of 5–10, against ~$35 for the parts it replaces. Keep the
footprint reusable if the board changes (H2).

### H2: Cheaper boards that protect the key: not now

**Goal.** Roughly halve the board cost, and make physical access no longer yield the link key
(`key-extract-physical`, `malicious-firmware-reflash`, accepted today because the SAMD21 can't prevent it).

**Today.** The MKR WAN 1310 is the costliest part, and its key sits in plain text in the external flash.

**How.** An STM32WL or SX1262 board (e.g. ESP32-S3 + SX1262, ~$20). The SX1262 brings lower receive current (~4.6 mA
against ~10 mA) and +22 dBm output, at about the SX1276's sensitivity for a matched SF and bandwidth; the ESP32-S3's
secure boot and flash encryption protect the key. Wi-Fi stays off.

Two concrete designs (2026-10-30, not decided): M5Stack StamPLC + Unit LoRaWAN-US915 in
[docs/proposals/m5stack-hardware.md](docs/proposals/m5stack-hardware.md), and RAK WisBlock (RAK3312 + RAK13001) in
[docs/proposals/rak-wisblock-hardware.md](docs/proposals/rak-wisblock-hardware.md), compared at the end of the latter.

**Cost / risk.** `radio.cpp` encodes a lot of hard-won SX1276 behaviour: a new radio driver, a flash and config
backend, esptool-js in place of SAM-BA in the web console, and the bench numbers measured again. Only worth it for many
sites; F11 lets it sit beside the MKR.

## Test bench

### B2: Test attacks over real RF

**Goal.** The replay, foreign-traffic, busy-channel and late-delivery defences are proven over real RF, not only in
the host simulation, including the command deadlines (0.16.0).

**Today.** Those paths are exercised only by the host tests.

**How.** The rejected board (…191117, `docs/bench-testing.md`) as a third node, all three in a shielded box (a metal tin): replay
frames, send foreign traffic on our SF and sync word, keep the channel busy, and withhold and late-deliver a CMD.

**Cost / risk.** Run it as a separate bench-only sketch, like the GateSim, never a mode in GateLink firmware
(`debug-commands-in-production`), and only at low power inside the box.

### B3: Reach the host-only fault paths on the bench

**Goal.** The fixes the bench can't trigger today are checked on real hardware too.

**Today.** The 0.13.9 paths (a radio fault during a pulse, five remote writes in one pulse, a switch flipped back
before the gate's next STATUS) are host-tested only (TODO.md).

**How.** A build flag compiling in fault injection (reset the radio mid-pulse, time a controller edge, replay
CFG_SETs); the release workflow checks it is off in release binaries. B2 covers the over-the-air ones.

### B5: Let the suite check D5, K1 and supply dips

**Goal.** The suite checks every output it depends on, and the supply dips behind the full-power TX resets can be
measured.

**Today.** The suite has no D5 check and checks K2 but not K1 after each test (TODO.md); supply dips are inferred from
resets.

**How.** D5 and house K1 wired to GateSim inputs; current sensing (an INA219, ~$5) on each board's 5 V feed.

### B6: Turn field incidents into tests

**Goal.** A problem seen at the site becomes a host test, instead of a bench reconstruction.

**Today.** A field incident has to be reproduced by hand on the bench.

**How.** Load a board's exported log (its event journal, `log.get` with `before`) and link history into the host
simulation.

## Development environment

### D2: Run the bench automatically, safely

**Goal.** Regressions on real hardware show up without someone starting the suite, and never at the cost of running
untrusted code on the bench PC.

**Today.** The bench suite runs only by hand.

**How.** A self-hosted runner on the bench PC running a smoke subset (`/flash`, preflight and `test_normal`) and
reporting `summary.json`, only on `workflow_dispatch` and pushes to `main` (decided 2026-10-12): never on
`pull_request` or a label, which would run a fork's code on the bench PC beside the key and the Home Assistant token.
A branch of the repository itself can be run by dispatching on it.

**Cost / risk.** Needs a port lock so it never runs while someone has the web console open.
