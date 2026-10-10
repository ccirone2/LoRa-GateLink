# Roadmap

Desired features and ideas. Bugs and test tasks go in [TODO.md](TODO.md); shipped features are in the
[release notes](https://github.com/ccirone2/LoRa-GateLink/releases).

Each item: a bold title, why it's wanted, and notes or open questions. Move an item up to **Next** when it's
decided, and remove it once it ships (the pull request and release notes describe it). Anything touching gate
behaviour must keep the behavioural invariants in [CLAUDE.md](CLAUDE.md).

## Next

_Nothing decided yet. Move items here from Ideas._

## Ideas

- **Use the spare inputs.** Gate IN4 and house IN3/IN4 are wired, debounced, logged and reported but have no
  behaviour. Candidates: a beam-break or vehicle-presence sensor at the gate reported to the house (and on to
  Alarm.com), or the alarm panel's armed state at the house. Needs a decision on what Alarm.com should see and
  whether any of it may affect commands (the opener's own safety inputs stay in charge of entrapment).
- **Tell Alarm.com about faults, not just "open".** Today `no_power`, `fault` and link loss all show as the
  contact sensor open and the switch on. A second 2GIG sensor (or a tamper/supervision input) driven by an extra
  relay could report "gate needs attention" separately. Needs a third relay output at the house.
- **Detect a Shelly that reboots without losing power.** IN2 only senses the Shelly's supply; an internal reboot
  can still produce an IN1 edge. Ideas: watch for the Shelly's relay dropping and coming back within a short
  window with no matching Alarm.com change, or sense the Shelly's status LED.
- **Battery backup and supply monitoring at the gate.** A LiPo on the MKR battery connector would keep the gate
  board reporting through an opener power cut; report its voltage (and the 5 V rail) in STATUS so the house can
  warn before it runs flat. Since 0.11.0 both boards read VIN power good from the charger chip (status `supply`);
  the gate could put it in STATUS. The chip has no ADC, so the LiPo voltage still needs a divider to an analog pin.
- **Longer or persistent link history.** The boards keep 96 hourly buckets in RAM (since 0.4.0), and every
  reset wipes them. Open questions:
  - Persist them? Hourly saves could go to the SPI flash that holds config since 0.5.0 (`extflash.cpp`; config
    uses sectors 0–1). RAM could at best carry them over warm resets (the linker script has no `.noinit`
    section; see the warm-reset item in [TODO.md](TODO.md)), never a power cut.
  - Go deeper? A week needs a slimmer bucket (64 B now) or 2 h buckets: a `config.get` reply takes ~5 KB of
    heap (`free_ram` in status).
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
- **Tamper switch on gate IN4** (`key-extract-physical`, `gate-input-spoofing`). An enclosure switch reported in
  STATUS, shown by the house as needs-attention and logged, so box access is noticed and the key rotated.
- **Plausibility checks at the gate** (`closed-limit-nc-fail-unsafe`, `power-sense-degradation-silent`,
  `reboot-loops`, `relay-held-welded-or-overlapping`). Latch faults that the inputs alone don't show: a closed limit
  that never releases after our OPEN pulse, closed reached without passing between, IN2 or IN3 off while everything
  else says power is there, repeated resets before the first full loop pass, a shared input held on while our coil
  is off.
- **Allow CLOSE without AC while a limit reads?** (`ac-cut-refuses-close`). Today every command is refused without
  AC, so cutting AC keeps the gate open. CLOSE is the secure direction. Decide, maybe behind a setting.
- **Verify the running image** (`tamper-undetectable`). Read the application back over the bootloader and compare
  its hash with the release's, from the tools and the web console.
- **Serve the console from its own origin** (`shared-github-io-origin`). A custom domain or a separate organisation,
  so other Pages sites of the account can't use its Web Serial grants. Needs the owner's domain or account.

