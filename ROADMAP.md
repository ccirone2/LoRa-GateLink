# Roadmap

Where GateLink is going: features, hardware, security and the development process, ranked in three tiers and
grouped by category within each. Bugs and bench/field tasks go in [TODO.md](TODO.md); shipped work is in the
[release notes](https://github.com/ccirone2/LoRa-GateLink/releases). Names in backticks such as
`jam-and-delay-replay-window` are threat ids in the [threat model](docs/threat-model.md).

Each item: a bold title, why it's wanted, and notes or open questions. Remove an item once it ships (the pull
request and release notes describe it). The owner sets the tiers; routines and agents never re-rank them, they ask
in the pull request instead. Anything touching gate behaviour must keep the behavioural invariants in
[CLAUDE.md](CLAUDE.md). The agent tooling (CLAUDE.md, skills, hooks, routines) is maintained with the same care as
the code.

- **Tier 1 — before the install.** Safety and security the field install relies on: the open high-severity threats.
- **Tier 2 — after the install.** Improvements that need field data, or close medium-severity threats.
- **Tier 3 — the next hardware revision and larger features.**

## Decisions

Settled; reopen one only with a new reason.

- **No latching relays, at the house or the gate.** An output must drop to its safe state whenever the board isn't
  running it. A latched contact would keep reporting a state nobody maintains any more (a stale source of truth for
  Alarm.com), which is worse than the short blip of a warm reset ([TODO.md](TODO.md)).
- **The house board always has a LiPo.** It keeps the board up through a 12 V cut, so it sees the controller's
  supply drop (`ctrl_power_pmic`) and keeps reporting.
- **A LiPo at the gate is optional.** Fit one only if the opener has no battery backup and the enclosure stays within
  the cell's charge and discharge temperature range (LiPo cells typically can't be charged below 0 °C; check the
  cell's datasheet).
- **No Alarm.com-compatible gate controller.** GateLink stays the bridge. Alarm.com sees the gate only through the
  Z-Wave controller and the 2GIG contact sensor(s).
- **Remote access can look but not act.** Anything that moves the gate, changes config or the key, or resets a board
  stays local-only (Tier 3, *Remote read-only web console*).

### Open questions for the owner

- **Allow CLOSE without AC while a limit reads?** (`ac-cut-refuses-close`, Tier 1). Today every command is refused
  without AC, so cutting AC keeps the gate open, and CLOSE is the secure direction. Decide, maybe behind a setting.
- **Serve the console from its own origin?** (`shared-github-io-origin`, Tier 2). A custom domain or a separate
  organisation, so other Pages sites of the account can't use its Web Serial grants. It needs the owner's domain or
  account, and the remote console (Tier 3) would want the same origin.
- **What does gate IN4 do?** The tamper switch (Tier 1), sensing a welded relay on the opener terminal
  (`relay-held-welded-or-overlapping`) and a beam-break sensor (Tier 3) all want it. Until the carrier PCB adds
  inputs, pick one.
- **PhotoMOS or mechanical outputs on the carrier PCB?** (Tier 3, with the checks listed there.)

## Tier 1 — before the install

### Security

- **Protocol hardening** (MINOR, both boards). One frame-format change for the open radio threats. The boards
  refuse the old format rather than fall back.
  - A 128-bit HELLO challenge bound to the challenger's session, and never re-verifying a session superseded since
    boot (`challenge-library-session-rebind`).
  - Freshness in CMD, e.g. the gate's uptime from its last STATUS echoed back, so a withheld command can't fire late
    (`jam-and-delay-replay-window`, `withheld-command-fires-late`).
  - MAC keys per session, derived from the challenge (`session-id-repeat`).
  - Optionally, payload encryption with per-direction keys, encrypt-then-MAC (`cleartext-traffic-analysis`).
- **Console lock** (`no-console-authentication-root`). An install-time setting after which state-changing console
  commands need proof of the key: `relay.test`, `config.*`, `key.set`, `remote.*` and `debug.*`. Status, logs and
  history stay open. Reflashing still bypasses it, but it stops casual USB actuation at the gate box.
  - Classify every console command once (open / locked), and have `tools/check_contract.py` require it.
  - The remote console (Tier 3) reuses the same classification.
- **Tamper switch on gate IN4** (`key-extract-physical`). An enclosure switch reported in
  STATUS, logged, and shown by the house as needs-attention (also on the fault output), so access to the box is
  noticed and the key rotated. See the IN4 question above.

### Safety and alarm reporting

- **Report a move the alarm missed** (`link-loss-concealment-window`). A "left closed since you last heard" latch or
  movement counter in STATUS. When the house sees it advance while it showed closed (including on link recovery), it
  opens K2 for a few seconds, and drops the fault output, so the alarm records the opening late rather than never.
- **Plausibility checks at the gate.** Latch the faults the inputs alone don't show, report them as `fault` and on
  the fault output:
  - a closed limit that never releases after our OPEN pulse, or closed reached without passing between
    (`closed-limit-nc-fail-unsafe`)
  - IN2 or IN3 off while everything else says power is there (`power-sense-degradation-silent`)
  - repeated resets before the first full loop pass, reported to the house (`reboot-loops`)
  - a shared input held on while our coil is off (`relay-held-welded-or-overlapping`)

## Tier 2 — after the install

### Alarm reporting

- **Finer fault reporting to Alarm.com.** Since 0.14.0, D5 (`fault_out`) gives the alarm one "gate needs attention"
  signal for every problem (radio, link, AC, `no_power`, `fault`), while the contact sensor still shows them as open.
  Still open:
  - Telling problems apart at the panel (a sensor per class, e.g. AC lost vs link lost). More outputs come with the
    carrier PCB.
  - Whether the controller's own power (IN2, the board's supply) should count at the house.
- **Detect a Shelly that reboots without losing power** (`shelly-reboot-spurious-commands`). IN2 senses only the
  Shelly's supply, so an internal reboot can still produce an IN1 edge. Ideas:
  - Watch for the Shelly's relay dropping and coming back within a short window with no matching Alarm.com change.
  - Sense the Shelly's status LED.

### Power

- **Battery and supply monitoring.** With a LiPo always at the house and sometimes at the gate:
  - Report each board's LiPo voltage, and the VIN power-good reading it already has (status `supply`, since 0.11.0).
    Send the gate's in STATUS, so the house can warn before a cell runs flat and show a gate on battery.
  - The charger chip has no ADC, so the voltage needs a divider to an analog pin: wire it now, or wait for the
    carrier PCB.
  - Add a temperature reading where a LiPo sits in an outdoor enclosure, and stop charging outside the cell's range
    if the charger allows it.

### Diagnostics

- **Longer link history, and a log floods can't wipe.** Since 0.14.0 each completed hourly bucket is also kept in
  the SPI flash, so a reset loses only the one in progress, but `hist.get` serves only the 96 a boot loads into RAM.
  - Go deeper? The flash log holds 512 buckets (three weeks of hours). `hist.get` could page the older ones from it
    in batches (each read takes the radio off the air ~0.5 s). Or RAM could take a slimmer bucket (68 B now) or 2 h
    buckets: a `config.get` reply takes ~5 KB of heap (`free_ram` in status).
  - Should the gate's own history be fetchable over LoRa (paged, like DIAG), or are the counters in STATUS enough?
  - Coalesce repeated flood events in the 64-entry log, so a replay flood can't overwrite the evidence
    (`replay-flood-erases-evidence-and-reflects`).

### Supply chain

- **Verify the running image** (`tamper-undetectable`). Read the application back over the bootloader and compare
  its hash with the release's, from the tools and from the web console.
- **Dependency integrity** (`arduino-deps-integrity`, `actions-tag-pinning`).
  - A hash lock for the Arduino core and libraries, checked in CI.
  - arduino-cli pinned in CI as it is in cloud sessions.
  - The release job's write token split from the build.

  The repository settings (rulesets, protected tags, immutable releases, attestations) are the owner's, in
  [TODO.md](TODO.md).

### Development process

- **Measure the improvement loop.** Once the nightly routines run ([TODO.md](TODO.md)), track what each produces:
  pull requests merged vs closed, findings that turned out wrong, and hook blocks that were false alarms. Sharpen or
  retire the routines and hooks that don't pay, and turn a workflow repeated by hand into a skill.
- **Check the trackers' cross-references.** Extend `tests/tools`:
  - Every threat id named in `ROADMAP.md`, `TODO.md` and the threat model exists in the threat model.
  - Every open threat that points to the roadmap has an item here.

  That keeps the three documents from drifting apart, as the docs-from-source checks do for the reference tables.

## Tier 3 — the next hardware revision and larger features

### Hardware

- **Carrier PCB.** It would replace the MKR Relay Proto Shield, the loose opto board, the buck and the fault-output
  relay module with one board per end. Pins move, so `firmware/GateLink/pins.h`, `docs/hardware.md`, the web
  console's `WIRING` table and the bench wiring record change together.
  - At least 4 outputs per board: OPEN, CLOSE and spares at the gate; the controller's SW input, the contact sensor,
    the fault sensor(s) and spares at the house.
  - More inputs than today's four, which settles the IN4 question.
  - Input conditioning on board: opto, pull-down, and TVS/RC on field wiring.
  - Screw terminals, the buck, the LiPo connector with a voltage divider (and a thermistor at the gate), and a UART
    header for bench power tests.
  - Room to keep the antenna away from switching parts (17 dBm with a relay energized has crashed a weakly powered
    board).
- **PhotoMOS outputs.** For the carrier PCB, consider PhotoMOS (MOSFET-output solid-state) relays instead of
  mechanical ones. Every output switches a low-level dry-contact signal (the CSW24UL OPEN/CLOSE inputs, the
  controller's SW input, 2GIG sensor loops), and that is what they suit: no bounce or contact wear, silent, a few mA
  of LED drive straight from a pin.
  - Like today's relays they are normally open and release when undriven. The fail-safe behaviour, the decisions
    above and `INTERLOCK_MS` don't change.
  - Check before choosing a part:
    - Off-state leakage against each input's threshold. The CSW24UL inputs are shared with the AES controller, so
      leakage must never look like a press.
    - On-resistance against each input's sensing current.
    - The voltage rating, with margin for surges on the gate's field wiring.
    - An AC/DC (bidirectional) output type, since the polarity at each input isn't guaranteed.
  - The CSW24UL's input ratings aren't in the documents we have: measure them on site, or ask LiftMaster.

### Inputs

- **Use the spare inputs.** Gate IN4 and house IN3/IN4 are wired, debounced, logged and reported but have no
  behaviour (gate IN4 may go to the tamper switch first). Candidates:
  - A beam-break or vehicle-presence sensor at the gate, reported to the house and on to Alarm.com.
  - The alarm panel's armed state at the house.
  - Sensing a welded relay on the opener terminal.

  Each needs a decision on what Alarm.com should see and whether any of it may affect commands. The opener's own
  safety inputs stay in charge of entrapment.

### Remote access

- **Remote read-only web console.** Reach the house board's console over the Internet to check status, logs, link
  history and config. Control and the other local-only actions are refused.
  - **The board enforces it, not the page.** A remote request arrives on a port the firmware knows is remote. That
    port accepts only the open commands from the console-lock classification (Tier 1): `info`, `status`,
    `config.get`, `log.get`, `hist.get`. Everything else is local-only, including firmware flashing.
  - **Decide on the diagnostics that transmit.** `radio.ping` and `remote.diag` only read, but they put frames on
    the air: allow them rate-limited, or keep them local.
  - **A network bridge.** The MKR WAN 1310 has no network, so it needs a small always-on host on the house board's
    USB or UART, or a network module on the carrier PCB. The bridge holds no key and can't widen what the port
    allows.
  - **Security.**
    - Authentication and encryption end to end.
    - No inbound port forwarding (an outbound tunnel or VPN).
    - Rate limits, and remote sessions logged.
    - The link key stays unreadable, as now.
    - Add the bridge and the remote session to the threat model before building it.
  - **The web console.** A transport besides Web Serial (e.g. a WebSocket) behind the existing board client, with
    the controls a remote session can't use hidden. The page's checks are a convenience; the board's are the
    guarantee.
