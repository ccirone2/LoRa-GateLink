# Roadmap

Desired features and ideas. Bugs and test tasks go in [TODO.md](TODO.md); shipped features are in the
[release notes](https://github.com/ccirone2/LoRa-GateLink/releases).

Each item: a bold title, why it's wanted, and notes or open questions. Move an item up to **Next** when it's
decided, and remove it once it ships (the pull request and release notes describe it). Anything touching gate
behaviour must keep the behavioural invariants in [CLAUDE.md](CLAUDE.md).

## Next

- **Allow CLOSE without AC while a limit reads** (`ac-cut-refuses-close`; decided 2026-10-12). Today the gate refuses
  every command while IN3 reads no AC (`RES_NO_POWER`, `role_gate.cpp`), because the opener may be on its last
  battery reserve and the AES controller sharing its inputs is unpowered. So cutting AC keeps an open gate open, and
  Alarm.com can't close it. CLOSE is the secure direction:
  - CLOSE runs as usual when a limit reads: it pulses from open (or to reverse an OPEN), and is `already` at closed.
  - OPEN stays refused without AC, and with no limit reading (`no_power`) both stay refused.
  - A battery too weak to finish the travel leaves the gate `no_power`, which the house already shows as not-closed.
  - No setting to keep the old behaviour (decided 2026-10-12): one behaviour, fewer knobs.
  - Gate only (the house handles the ACK as any other), so a PATCH. Update with it the AC-power invariant in
    CLAUDE.md, the README's "How it behaves", `docs/hardware.md` (IN3), the `power_sense` help in
    `web/js/settings.js`, and the threat model. Host tests: CLOSE without AC from open pulses, OPEN refuses,
    `no_power` refuses both, and the house's resync after a refused OPEN still happens. Bench: the power tests.

## Ideas

- **Use the spare inputs.** Gate IN4 and house IN3/IN4 are wired, debounced, logged and reported but have no
  behaviour. Candidates: a beam-break or vehicle-presence sensor at the gate reported to the house (and on to
  Alarm.com), or the alarm panel's armed state at the house. Needs a decision on what Alarm.com should see and
  whether any of it may affect commands (the opener's own safety inputs stay in charge of entrapment). Gate IN4 is
  the gate's only spare and other ideas below want it too (the independent closed sensor, the tamper switch, and
  sensing the opener's OPEN/CLOSE terminals for a welded contact): pick one, or add inputs (the carrier board, under
  Hardware).
- **Finer fault reporting to Alarm.com** (`jamming-hides-state-until-link-timeout`). Since 0.14.0 D5 (`fault_out`)
  gives the alarm one "gate needs attention" signal for every problem (radio, link, AC, `no_power`, `fault`),
  alongside the contact sensor still showing them as open. Still open: telling problems apart at the panel (a sensor
  per class, e.g. AC lost vs link lost), and whether the controller's own power (IN2, the board's supply) should count
  at the house.
- **Detect a Shelly that reboots without losing power** (`shelly-reboot-spurious-commands`). IN2 only senses the
  Shelly's supply; an internal reboot can still produce an IN1 edge. Ideas: watch for the Shelly's relay dropping and
  coming back within a short window with no matching Alarm.com change, or sense the Shelly's status LED.
- **Battery backup and supply monitoring at the gate.** A LiPo on the MKR battery connector would keep the gate
  board reporting through an opener power cut; report its voltage (and the 5 V rail) in STATUS so the house can
  warn before it runs flat. Since 0.11.0 both boards read VIN power good from the charger chip (status `supply`);
  the gate could put it in STATUS. The chip has no ADC, so the LiPo voltage still needs a divider to an analog pin.
  A second divider on the opener's 24 V accessory rail would show the opener's own battery sagging through an AC
  outage, so the house can warn of "opener battery low" before the gate goes `no_power` (check how the buck shares
  ground with that rail first). Diagnostics only: in the history and STATUS, never in a decision.
- **Independent closed sensor at the gate** (`closed-limit-nc-fail-unsafe`, High). "Closed" today rests on one
  contact, the AUX2 relay's NC, which reads closed if that relay fails with its 24 V present. A magnetic or reed
  sensor on the leaf (a few dollars) as a second closed input: the gate reports closed only when both agree, and a
  disagreement that lasts reads `fault`, which the house already shows as not-closed. Wants a spare input (gate IN4
  is contended, see the spare inputs) and a host-test monitor that closed never shows with either sensor open.
- **Travel-time record.** Log how long each open and close takes (pulse or limit release to the other limit) and
  keep it in the history: a gate slowing over weeks points at a weak opener battery or a mechanical problem, and the
  measured times show what `travel_timeout_s` should be. Diagnostics only. Open question: suggest a
  `travel_timeout_s` in the web console from the record, rather than setting it by itself.
- **Controllers that pulse instead of holding a switch.** Many smart relays and garage controllers give a momentary
  contact rather than an on/off level, so supporting one would let the house work with far more than the Shelly
  (the web console already calls it the generic "controller"). Needs a design for how a pulse maps to OPEN/CLOSE
  (toggle against the gate's state is ambiguous mid-travel), how K1 sync works without a level to mirror, and how
  it keeps the power-sense and sync-window rules.
- **More than one gate per house** (e.g. a pedestrian gate beside the drive gate). Node addresses are fixed by role
  (house 1, gate 2) and STATUS carries one gate; one-to-many is a frame-format change, so decide it before 1.0
  fixes the wire format. Each gate would need its own controller input, contact sensor and fault output at the
  house, which the relay shield doesn't have (see the carrier board).
- **Signed firmware updates over LoRa.** The gate board is outdoors and every update means a laptop at the gate
  box. The 2 MB SPI flash can stage an image (~120 KB: minutes at SF9, 500 kHz, paced so the link keeps working),
  checked against an Ed25519 signature (the Crypto library has it) before a small bootloader copies it into place.
  Unsigned, it would be a remote takeover path, so the signature is the point. Never during a relay pulse; the
  house sends, and only to a gate that has reported its version. The largest and riskiest item here: it needs its
  own threat-model section and power cuts mid-copy covered by the host tests.

- **Longer link history.** Since 0.14.0 each completed hourly bucket is kept in the SPI flash as well, so a reset
  loses only the one in progress, but `hist.get` serves only the 96 a boot loads into RAM. Open questions:
  - Go deeper? The flash log holds 512 buckets (three weeks of hours): `hist.get` could page the older ones from
    it (each read takes the radio off the air ~0.5 s, so in batches), or RAM could take a slimmer bucket (68 B
    now) or 2 h buckets: a `config.get` reply takes ~5 KB of heap (`free_ram` in status).
  - Should the gate's own history be fetchable over LoRa (paged, like DIAG), or are the counters in STATUS
    enough?
- **Protocol hardening** (MINOR, both boards; [threat model](docs/threat-model.md)). One frame-format change for the
  open radio threats: a 128-bit HELLO challenge bound to the challenger's session, and never re-verifying a session
  superseded since boot (`challenge-library-session-rebind`); freshness in CMD, e.g. the gate's uptime from its last
  STATUS echoed back, so a withheld command can't fire late (`jam-and-delay-replay-window`,
  `withheld-command-fires-late`); MAC keys per session derived from the challenge (`session-id-repeat`); optionally
  payload encryption with per-direction keys, encrypt-then-MAC (`cleartext-traffic-analysis`). The boards refuse the
  old format rather than fall back.
- **Report a move the alarm missed** (`link-loss-concealment-window`). A "left closed since you last heard" latch or
  movement counter in STATUS: when the house sees it advance while it showed closed (also on link recovery), it opens
  K2 for a few seconds (and the fault output) so the alarm records the opening late rather than never.
- **Console lock** (`no-console-authentication-root`). An install-time setting after which state-changing console
  commands (relay.test, config.*, key.set, remote.*, debug.*) need proof of the key, leaving status, logs and history
  open. Reflashing still bypasses it; it stops casual USB actuation at the gate box.
- **Tamper switch on gate IN4** (`key-extract-physical`, `malicious-firmware-reflash`). An enclosure switch reported
  in STATUS, shown by the house as needs-attention and logged, so box access is noticed and the key rotated.
- **Plausibility checks at the gate** (`closed-limit-nc-fail-unsafe`, `power-sense-degradation-silent`,
  `reboot-loops`, `relay-held-welded-or-overlapping`). Latch faults that the inputs alone don't show: a closed limit
  that never releases after our OPEN pulse, closed reached without passing between, IN2 or IN3 off while everything
  else says power is there, repeated resets before the first full loop pass, a shared input held on while our coil is
  off (that last one needs the opener's OPEN/CLOSE terminals sensed on an input: gate IN4, see the spare inputs).
- **Keep the evidence of a replay flood** (`replay-flood-erases-evidence-and-reflects`). A flood of replayed frames
  overwrites the 64-entry event log, and the persistent hourly history (0.14.0) counts MAC failures but not replays.
  Coalesce repeated flood events into one log entry with a count, add replays to the history bucket (a `HLOG_FMT`
  bump), and limit how often a memo'd seq is re-ACKed.
- **Verify the running image** (`tamper-undetectable`). Flashing already checks what it wrote (the web console's
  SAM-BA CRC, bossac's verify); this is a check of a board at any later visit: read the application back over the
  bootloader and compare it with the release's image, from the tools and the web console.
- **Serve the console from its own origin** (`shared-github-io-origin`). A custom domain or a separate organisation,
  so other Pages sites of the account can't use its Web Serial grants. Needs the owner's domain or account. Deferred
  (2026-10-12): maybe eventually.

## Hardware

The parts for one end cost about $110–125 today (MKR WAN 1310 ~$50–55, relay shield ~$22, antenna and pigtail,
opto board, buck, LiPo, enclosure). Keep a location at or under ~$120: an upgrade on the current boards has to stay
cheap (~$15 or less) or pay for itself by replacing parts.

- **Carrier board.** One PCB under the MKR replacing the relay shield, the opto board, the buck and the loose
  wiring: optos with the PNP-equivalent polarity the inputs need, TVS on every field input, a 6–36 V input supply,
  screw terminals labelled per role, the D5 output with its relay or opto, the welded-contact sense and tamper switch
  inputs, more inputs than IN1–IN4 (they're contended), and a bulk capacitor. Assembled in small batches it should
  cost less than the ~$34 of parts it replaces, and it removes most of the polarity and voltage warnings in
  `docs/hardware.md`. Keep the footprint reusable if the board changes (below).
- **Latching relays for the house outputs.** The warm-reset blip of house K1/K2 (TODO.md) can't be fixed in firmware
  because the bootloader holds the pins released ~0.5 s; bistable relays would keep the contact sensor and the
  controller's input through a reset. House only: a latching relay at the gate would stay closed after a crash,
  breaking the pulse-only invariant. Open question: what the house shows if it boots to a relay state it can't read
  back (sense the contacts, or always set them once the first STATUS arrives).
- **Surge and weather protection at the gate.** The gate box is outdoors on long runs to the opener: TVS or RC on
  the inputs and relay contacts (on the carrier board, or a terminal strip until then), a grounded bulkhead for the
  antenna, a gasketed enclosure with cable glands. Record what was fitted in `docs/hardware.md`.
- **A second board target, later.** ESP32-S3 + SX1262 boards (e.g. Heltec WiFi LoRa 32 V3, ~$20) cost about $30 less
  per end, receive a few dB better and draw less in RX, and the ESP32-S3's secure boot and flash encryption would
  stop physical access from yielding the key (`key-extract-physical`, `malicious-firmware-reflash`, accepted today
  because the SAMD21 can't). The cost: a new `radio.cpp` (SX126x), a flash and config backend, esptool-js in place
  of SAM-BA in the web console, and the bench numbers measured again; Wi-Fi stays off. Not before 1.0 on the MKR;
  meanwhile keep the chip-specific code behind `board.h`, `radio` and `extflash` so a second target can sit beside
  the first rather than replace it.

## Bench and development

- **Python on uv.** A `pyproject.toml` and `uv.lock` for `tools/`, `tests/e2e` and `tests/tools` (today
  `tests/e2e/requirements.txt` has lower bounds only), with CI running `uv sync --locked`. The lock carries hashes,
  which closes `python-deps-unpinned` (TODO.md, bench hygiene). Update `docs/development.md`, the skills and
  `tools/agent/cloud_setup.sh` with it.
- **The bench in CI.** A self-hosted runner on the bench PC running a short e2e smoke subset nightly and on demand,
  reporting `summary.json`. Only on pushes to `main` and manual runs, never on `pull_request`: the repository is
  public, and a fork's pull request must never run on the bench PC. Needs a smoke marker and a port lock so it
  never runs while someone has the web console open.
- **A repeatable RF rig.** A step attenuator and a shielded box (bench equipment, outside the per-location budget)
  so `-m rf` runs at a known path loss instead of with the antennas unplugged; covers the bench half of "Real RF"
  in TODO.md and gives `rftest` a calibrated pass line.
- **Fault hooks for bench builds.** A build flag compiling in fault injection (reset the radio mid-pulse, replay
  CFG_SETs, time a controller edge) for the 0.13.9 paths only the host tests reach (TODO.md); the release workflow
  checks it is off in release binaries.
- **More GateSim senses.** Current sensing (an INA219, ~$5) on each board's 5 V feed to measure the dips behind the
  full-power TX resets; D5 and house K1 wired to GateSim inputs so the suite checks them (TODO.md lists both gaps);
  a cheap logic analyzer with sigrok for the D5 glitch-at-reset check.
- **Replay field logs in the simulation.** Load a board's exported log and link history into the host simulation
  so a problem seen at the site becomes a test, instead of a bench reconstruction.
