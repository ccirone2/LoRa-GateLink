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
| **Tier 1: the gate's real state, safety, never late** | | | | |
| 1 | F20 | Let Alarm.com close the gate during an AC outage | S | |
| 2 | F12 | Never report closed on one failed contact | M | an input (H1, or gate IN4) |
| 3 | F14 | Catch faults the inputs alone don't show | M | |
| 4 | F17 | Tell the alarm about a move it missed | M | best with F6 (one frame change) |
| 5 | F6 | Keep the link private and commands fresh | L | decide F8's addressing with it |
| **Tier 2: remote diagnosis, the installer's work, robust hardware** | | | | |
| 6 | C3 | Help the installer aim the antenna | S | |
| 7 | B1 | Repeatable RF tests | S | |
| 8 | F3 | Tell the alarm what kind of trouble it is | M | |
| 9 | F1 | Keep the event log across resets | M | |
| 10 | F2 | See the gate's log, history and status from the house | L | F1 for the log |
| 11 | H1 | Cheaper ends that are hard to wire wrong | L | settles F3, F4, F12's parts |
| 12 | F7 | Update the gate's firmware without visiting it | XL | F1, F2 |
| **Tier 3: worthwhile, less urgent** | | | | |
| 13 | F4 | Warn before the opener's battery or mechanics fail | S | a divider (H1 or by hand) |
| 14 | F19 | Notice when a box is opened | S | an input (H1, or gate IN4) |
| 15 | B5 | Let the suite check D5, K1 and supply dips | S | |
| 16 | C2 | Answer "what happened last night?" | M | F1, F2 |
| 17 | F11 | Firmware ready for a second board | M | |
| 18 | B2 | Test attacks over real RF | M | B1 |
| 19 | B3 | Reach the host-only fault paths on the bench | M | |
| 20 | D2 | Run the bench automatically, safely | M | |
| 21 | F18 | Stop casual actuation over USB | M | |
| 22 | F16 | Ignore a Shelly that reboots without losing power | M | |
| 23 | F5 | Change radio settings without stranding the link | L | |
| 24 | F13 | Work with controllers that pulse | L | |
| 25 | F8 | Serve more than one gate from one house | L | the addressing decided with F6 |
| **Tier 4: later, or waiting on a decision** | | | | |
| 26 | C5 | Keep other sites from using the console's port grants | S | the owner's domain |
| 27 | F15 | Put the spare inputs to use | S | allocate inputs first (F12, F19, F14) |
| 28 | C4 | Check a board still runs genuine firmware | M | |
| 29 | B6 | Turn field incidents into tests | M | F1 |
| 30 | H2 | Cheaper boards that protect the key: not now | XL | F11 |

## Firmware

### F1: Keep the event log across resets

**Goal.** What happened before a reset, a power cut or an attack can still be read afterwards: the developer and
installer get a lasting record of events, not just the hourly link history.

**Today.** The hourly history persists in flash since 0.14.0, but log events live only in a 64-entry RAM ring stamped
with `millis()`: a reset loses them, and a flood of replayed frames overwrites them
(`replay-flood-erases-evidence-and-reflects`).

**How.**
- Write events to the unused flash sectors from 20 up (config 0–1, boot counter 2–3, history 4–19) with
  `histlog.cpp`'s record envelope; leave room for F7's staged image when allocating them.
- Stamp each event with the boot count and uptime; record wall-clock time when the web console connects, so the
  console can show real times.
- Keep the RAM ring in `.noinit` memory, so the last events before a watchdog reset are written at the next boot.
- Let `log.get` page older entries.
- Keep a replay flood's evidence: coalesce repeated flood events into one entry with a count, add replays to the
  history bucket (a `HLOG_FMT` bump), and limit how often a memo'd seq is re-ACKed.

**Cost / risk.** Each flash access stalls the loop ~0.5 s with the radio off the air, so batch the writes and never
write during a pulse (as `histSave`). A PATCH. C2 shows it as a timeline.

### F2: See the gate's log, history and status from the house

**Goal.** Diagnose the gate end without a trip to the gate box: from the house board's USB, the installer and
developer see what the gate has seen.

**Today.** `remote.diag` returns only the gate's firmware version, uptime, six counters and its remote-writable
settings, and `hist.get` serves only the 96 hourly buckets a boot loads into RAM, though the flash log holds 512
(three weeks).

**How.** Let the house page:
- the gate's log (from flash once F1 exists);
- its link history, and older buckets of either board's history from the flash log, read in batches since each read
  takes the radio off the air ~0.5 s (or keep slimmer buckets in RAM, 68 B now, or 2 h ones: a `config.get` reply
  takes ~5 KB of heap);
- its full status: health, supply, AC, `loop_max_us`, `free_ram`, and `crash_resets` with the last hang (`hang_stage`,
  `hang_pc`; since 0.15.0 the house learns only the gate's reboot loop, STATUS bit 7).

The gate answers only with no command pending and no relay pulsing, and pages are rate-limited.

**Cost / risk.** New message types: a MINOR both boards need together. Update the console contract and the threat
model (payloads stay readable until F6). C2 uses this data.

### F3: Tell the alarm what kind of trouble it is

**Goal.** The homeowner's alarm app says which problem the gate has (jammed, AC lost, link lost, a board stuck
rebooting), not only that it needs attention, and covers every problem that leaves the gate unattended
(`jamming-hides-state-until-link-timeout`).

**Today.** Since 0.14.0 D5 (`fault_out`) drives one second 2GIG sensor for radio, link, AC, `no_power` and `fault`
problems, and since 0.15.0 a reboot loop at either board (health `reboots`), all alike.

**How.**
- Add a travel timeout (jam): the house already gets `last_result` in STATUS.
- A sensor per problem class, e.g. AC lost vs link lost.
- Hardware: an AQY212EH PhotoMOS (~$2) could replace the relay module, plus $25–35 per extra 2GIG sensor. It needs
  up to 3 mA (5–10 mA recommended) and the SAMD core leaves high drive strength off, so either set it on D5 with
  ~330 Ω or add a transistor. Part of H1.

**Open questions.** How long a jam stays latched; whether the house controller's own power (IN2, the board's supply)
counts; how many sensors are worth their cost.

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

### F6: Keep the link private and commands fresh

**Goal.** Someone listening on 915 MHz can't learn when the gate opens or the mains power is out, and a command can
never fire later than the owner meant, even if it was jammed and recorded. Closes the open radio threats in the
[threat model](docs/threat-model.md).

**Today.** Payloads are readable, which is useful to a burglar (`cleartext-traffic-analysis`). A CMD jammed at the gate
and recorded can still be delivered later inside the replay window (`jam-and-delay-replay-window`,
`withheld-command-fires-late`). HELLO challenges are 32 bits and not bound to a session
(`challenge-library-session-rebind`).

**How.** One frame-format change, which the boards refuse to fall back from:
- Encryption: AES-CTR (the Crypto library has AES), encrypt-then-MAC, with per-direction keys derived per session from
  the challenge (`session-id-repeat`), so a CTR nonce of session and seq never repeats under one key even if a session
  id does.
- Freshness: encryption doesn't stop a late delivery (the frame still decrypts and verifies), so stamp each CMD with
  the gate's session clock, which the house estimates from STATUS, and have the gate refuse any CMD older than
  `cmd_ttl_s`.
- Challenges: 128 bits, bound to the challenger's session, and never re-verify a session superseded since boot.

**Cost / risk.** A MINOR both boards need together. Decide F8's addressing and F17's STATUS field in the same change,
so the wire format changes once before 1.0.

### F7: Update the gate's firmware without visiting it

**Goal.** A new firmware reaches the gate board from the house, safely, so the outdoor end never needs a laptop at the
gate box for an update. The biggest saving in field effort.

**Today.** Every update means opening the gate box and connecting over USB.

**How.**
- Stage the image in the SPI flash (~120 KB of the free sectors, allocated with F1).
- Accept only Ed25519-signed images (the Crypto library has it), with the private key off both boards: an unsigned or
  link-key-signed path would be a remote takeover route.
- The house sends, and only to a gate that has reported its version. Apply only when the gate is idle with AC on,
  never during a pulse; confirm or roll back after boot.
- Pace it so the link keeps working: a ~120 KB image at SF9 / 500 kHz (~7 kbps raw) is ~2.5 minutes before protocol
  overhead.

**Cost / risk.** The most work and the most risk here: a failed update strands a board at the gate. Do F1 and F2 first
so a failed update can be diagnosed. It needs a bootloader stage that copies the staged image into place. Give
it its own threat-model section, and host-test power cuts mid-copy.

**Open questions.** Does Arduino's SFU loader support this board (not confirmed)?

### F8: Serve more than one gate from one house

**Goal.** One house end controls and reports several gates, e.g. a pedestrian gate beside the drive gate: the obvious
growth case.

**Today.** Node addresses are fixed by role (house 1, gate 2) and STATUS carries one gate.

**How.** Make addressing one-to-many. Each gate needs its own controller input, contact sensor and fault output at the
house, which the relay shield doesn't have (H1).

**Cost / risk.** A protocol change, so decide the addressing with F6, before 1.0, even if it's built later. Each extra
gate end costs about $100.

### F11: Firmware ready for a second board

**Goal.** A second hardware target (H2) can be added beside the MKR rather than replacing it.

**Today.** Most chip-specific code already sits behind `board.h` (the host simulation builds everything from `app.cpp`
down except the radio, flash, charger and console drivers), but nothing keeps it that way.

**How.** Keep everything that touches the chip behind `board.h`, `radio` and `extflash`, so the rest of the firmware
never touches the hardware directly.

**Cost / risk.** A refactor with no behaviour change: the host tests pass unchanged.

### F12: Never report closed on one failed contact

**Goal.** The alarm sees "closed" only when the gate really is closed, even if the opener's limit relay fails
(`closed-limit-nc-fail-unsafe`, High).

**Today.** "Closed" rests on one contact, the AUX2 relay's NC, which reads closed if that relay fails with its 24 V
present.

**How.** A magnetic or reed sensor on the leaf (a few dollars) as a second closed input: the gate reports closed only
when both agree, and a lasting disagreement reads `fault`, which the house already shows as not-closed. A host-test
monitor checks closed never shows with either sensor open.

**Cost / risk.** Needs an input: gate IN4 is contended (F15), or H1's extra inputs.

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

**Goal.** A failing limit, power sense or relay contact is reported as a fault instead of quietly giving a wrong state
or disabling remote control (`closed-limit-nc-fail-unsafe`, `power-sense-degradation-silent`,
`relay-held-welded-or-overlapping`).

**Today.** The gate trusts each input as it reads; a weld or a stuck limit is invisible.

**How.** Latch a fault for: a closed limit that never releases after our OPEN pulse; closed reached without passing
between; IN2 or IN3 off while everything else says power is there; a shared input held on while our coil is off (that
last needs the opener's OPEN/CLOSE terminals sensed on an input: F15, or H1). Reboot loops are counted since 0.15.0
(health `reboots`).

### F15: Put the spare inputs to use

**Goal.** The spare inputs carry something worth knowing, e.g. a vehicle at the gate, or the alarm's armed state at
the house.

**Today.** Gate IN4 and house IN3/IN4 are wired, debounced, logged and reported but have no behaviour.

**How.** Candidates: a beam-break or vehicle-presence sensor at the gate reported to the house (and on to Alarm.com),
or the alarm panel's armed state at the house. Gate IN4 is the gate's only spare, and F12, F14 and F19 want it too:
allocate it, or add inputs (H1).

**Open questions.** What should Alarm.com see, and may any of it affect commands? (The opener's own safety inputs stay
in charge of entrapment.)

### F16: Ignore a Shelly that reboots without losing power

**Goal.** An internal Shelly reboot never moves the gate (`shelly-reboot-spurious-commands`).

**Today.** IN2 only senses the Shelly's supply, so a reboot with power present can still produce an IN1 edge, a CLOSE
then an OPEN.

**How.** Ideas: watch for the Shelly's relay dropping and coming back within a short window with no matching Alarm.com
change, or sense its status LED.

### F17: Tell the alarm about a move it missed

**Goal.** An opening hidden by jamming or a cut gate board is recorded by the alarm late rather than never
(`link-loss-concealment-window`).

**Today.** Jamming through an open–pass–close cycle leaves the sensor closed until the link timeout.

**How.** A "left closed since you last heard" latch or movement counter in STATUS: when the house sees it advance while
it showed closed (also on link recovery), it opens K2 for a few seconds (and D5).

**Cost / risk.** A STATUS change: ship it with F6.

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

**How.** An enclosure switch at the gate (IN4, or an H1 input), reported in STATUS, shown by the house as
needs-attention and logged.

### F20: Let Alarm.com close the gate during an AC outage

**Goal.** Cutting the AC can't keep an open gate open: Alarm.com can still close it while the opener runs on its
battery (`ac-cut-refuses-close`; decided 2026-10-12). CLOSE is the secure direction.

**Today.** The gate refuses every command while IN3 reads no AC (`RES_NO_POWER`, `role_gate.cpp`), because the opener
may be on its last battery reserve and the AES controller sharing its inputs is unpowered.

**How.**
- CLOSE runs as usual when a limit reads: it pulses from open (or to reverse an OPEN), and is `already` at closed.
- OPEN stays refused without AC, and with no limit reading (`no_power`) both stay refused.
- A battery too weak to finish the travel leaves the gate `no_power`, which the house already shows as not-closed.
- No setting to keep the old behaviour (decided 2026-10-12): one behaviour, fewer knobs.

**Cost / risk.** Gate only (the house handles the ACK as any other), so a PATCH. Update with it the AC-power invariant
in CLAUDE.md, the README's "How it behaves", `docs/hardware.md` (IN3), the `power_sense` help in `web/js/settings.js`,
and the threat model. Host tests: CLOSE without AC from open pulses, OPEN refuses, `no_power` refuses both, and the
house's resync after a refused OPEN still happens. Bench: the power tests.

## Web console

### C2: Answer "what happened last night?"

**Goal.** The installer or developer reads the site's recent events as a story, from the house, without a laptop at
the gate.

**Today.** The log is a list of coded events from one board, lost at a reset.

**How.** The F1 journal as a readable timeline, e.g. "14:02 Opened from the controller, open in 14 s", "03:10 Moved
locally (keypad or other controller)", readable remotely through the house via F2. (The naming rule applies: no brand
names in `web/`.)

### C3: Help the installer aim the antenna

**Goal.** At the install, the installer finds the antenna's best spot quickly, by watching the margin change live.

**Today.** The survey card and `tools/gatelink.py survey` give a verdict only after a timed run.

**How.** An open-ended *Aim* mode that pings every 2 s until stopped and shows the latest margin above the SF floor in
both directions in large type, coloured by the survey's good/fair thresholds (the same `survey.js` / `survey.py` logic
and shared vectors), and a matching `survey --live`. The timed survey then runs for the install record.

### C4: Check a board still runs genuine firmware

**Goal.** At any service visit, the installer can confirm a board runs the release it should (`tamper-undetectable`).

**Today.** Flashing checks what it wrote (the web console's SAM-BA CRC, bossac's verify), but nothing checks a board
later.

**How.** Read the application back over the bootloader and compare it with the release's image, from the tools and the
web console.

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
also gives the inputs and outputs F3, F4, F8, F12, F14, F15 and F19 need. The hardware change most worth making.

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

**Cost / risk.** `radio.cpp` encodes a lot of hard-won SX1276 behaviour: a new radio driver, a flash and config
backend, esptool-js in place of SAM-BA in the web console, and the bench numbers measured again. Only worth it for many
sites; F11 lets it sit beside the MKR.

## Test bench

### B1: Repeatable RF tests

**Goal.** RF tests run at a known, repeatable link margin, so results compare from run to run and boards can be passed
or failed against a calibrated line.

**Today.** `-m rf` runs with the antennas unplugged or at low power, and the margin varies with the bench layout; the
bench half of "Real RF" in TODO.md is open.

**How.** Fixed SMA attenuators and a shielded box (a metal tin works; about $30, bench equipment outside the
per-location budget), and a calibrated pass line for `rftest`.

### B2: Test attacks over real RF

**Goal.** The replay, foreign-traffic, busy-channel and late-delivery defences are proven over real RF, not only in
the host simulation, including F6's freshness check.

**Today.** Those paths are exercised only by the host tests.

**How.** The rejected board (…191117, TODO.md / `docs/bench-testing.md`) as a third node inside B1's box: replay
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

**How.** Load a board's exported log (the F1 journal) and link history into the host simulation.

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
