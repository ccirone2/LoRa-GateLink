# Threat model

What GateLink protects, from whom, what it guarantees today, and what it leaves to the install. It is meant to be
read before changing the link, the console, the release pipeline or the install procedure, and kept current with
them: a pull request that changes one of these says which threats below it affects (by id).

It comes from a review of the code at 0.13.6 (October 2026). Analysts each took one attack surface, and every high and
medium threat was then checked against the code by an independent reviewer: refuted claims were dropped and overstated
ones corrected. Nothing here was attacked on real hardware; probabilities are calculations.

## The system and its trust boundaries

```
 Alarm.com ── Z-Wave ── Shelly Wave 1 ──IN1/K1── HOUSE board ═══ LoRa 915 MHz ═══ GATE board ──K1/K2── CSW24UL opener
 (cloud, app)            (12 V, indoors)         (indoors, K2 ── 2GIG sensor)      (outdoor box)        ▲ also driven by
                                                                                                        AES Prime Edge,
 Your PC: web console (GitHub Pages) or tools/gatelink.py ── USB ── either board                       siren sensor,
 GitHub: source, CI, releases, Pages                                                                    local controls
```

| Boundary | Trusted side | Notes |
|---|---|---|
| The radio link | The two boards, which share one 128-bit key | Anyone in range can listen, record, replay, inject and jam. Range can be kilometres. |
| A board's USB port, UART pins, SWD pads, SPI flash | **Inside**: physical or USB access to a board equals full control of it, including its key | The console has no authentication by design. |
| The gate box | Reachable by someone at the gate | Often from the unsecured side. |
| The house box | Indoors | |
| The controller (Shelly, Z-Wave, Alarm.com) | Trusted to report the owner's intent | GateLink acts on every switch edge it sees (`upstream-controller-trusted`). |
| The opener's other inputs (AES, siren, buttons) | Outside GateLink | GateLink reports their movements; it can't prevent them. |
| The PC, the web console, GitHub | Trusted to deliver genuine firmware and to generate and hold keys | See "Supply chain". |

## Assets

1. **Gate actuation:** a pulse only for a fresh command the owner just gave, never late and never twice.
2. **What the alarm sees:** the contact sensor (K2) closed only while the gate is closed and the link is up; the
   Alarm.com switch (K1) following the real gate.
3. **The link key.** With it, an attacker can command the gate and forge its state.
4. **Availability** of remote control, and the fail-safe when it is lost (sensor open).
5. **Privacy:** when the gate opens, when the property is in use, when mains power is out.
6. **Evidence:** the log, counters and link history that show an attack after the fact.

## Adversaries

| Adversary | Can | Can't (assumed) |
|---|---|---|
| **RF attacker** | Listen, record, replay, inject and jam on 915 MHz with an SDR; jam one end while recording the other ("RollJam"); leave a device running for weeks | Know the key; touch the boards |
| **Someone at a box** | Plug into USB, reach the UART pins, clip the flash chip, rewire inputs, cut power | Is assumed to be noticed eventually (enclosure, tamper evidence) |
| **Remote software attacker** | Compromise a GitHub account, a Pages deploy, a dependency, or the PC that holds the key | Break GitHub, Pages TLS or the hosted runners themselves |
| **Faults and time** | Power cuts, brown-outs, crashes, a flaky opto or wire, years of uptime, neighbours' LoRa traffic | |

The repository is public: every attacker knows the frame format, the defaults and the state machines.

## What GateLink guarantees today

- **No forged frames.** Every frame carries HMAC-SHA256 truncated to 64 bits, checked (in constant time) before any
  payload is parsed. Without the key no command, state or config write can be forged (`frame-forgery`: low).
- **No replay of frames that were already delivered.** Each boot draws a fresh random session id and starting
  seq. A peer's session counts only once it has echoed a fresh challenge, and from then on only seqs above that,
  each once (32-frame window). Delivered frames can't be replayed, within a session or across reboots
  (`recorded-frame-replay`: low).
  - The exceptions are frames that were **withheld** (jammed at the receiver and recorded) and replayed later, and a
    **library of recorded challenge answers**. Both are open; see below.
- **No duplicate pulses** for one command, whatever is lost or restarted. The gate de-duplicates by command id, and
  a command pending across a gate restart is dropped, not resent.
- **Fail-safe outputs.** Relays drop on any reset. The gate's inputs are pulled down, so a dead opto or cut wire
  never reads as a limit. The house never commands at boot. The contact sensor fails open when the link is lost:
  after max(`link_timeout_s`, 2.5 × the gate's `heartbeat_s`), 100 s at the defaults.
- **The key can't be read back** over the console or the link, and the link stays off until a key is set.

It does **not** guarantee:

- **Confidentiality.** Payloads and message types are plain text, and frame lengths are distinct
  (`cleartext-traffic-analysis`: medium).
- **Availability under jamming.** It can only fail safe and make the jamming visible (`rf-jamming-dos`,
  `jamming-hides-state-until-link-timeout`).
- **Anything against someone with access to a board.**

## Threats

Severity is after verification: **H**igh, **M**edium, **L**ow. Status says where each stands; open items link to
[TODO.md](../TODO.md) (bugs and tasks) or [ROADMAP.md](../ROADMAP.md) (features and decisions).

### Radio link

| Id | Threat | Sev | Status |
|---|---|---|---|
| `hello-answer-stamp-deadlock` | After ~24.9 days of steady link, a peer restart's HELLOs were ignored for up to another ~24.9 days (the HELLO answer stamps read as recent under the signed `elapsed()`): link down, sensor open | H | **Fixed** in 0.13.7, with host tests past 25 days (link PR) |
| `backoff-starvation-by-frequent-frames` | Any LoRa frame heard every ~100 ms held all new frames off until their TTL: a cheap, silent DoS | M | **Fixed** in 0.13.7 (link PR) |
| `jam-and-delay-replay-window` / `withheld-command-fires-late` | A command jammed at the gate and recorded can be replayed later, while still inside the 32-frame window (about 16 minutes of normal traffic): the gate opens at a time the attacker picks, after the house had dropped the command. The receiver can't tell a delayed frame from a retry | H | Open: protocol hardening (CMD carries freshness; ROADMAP) |
| `withheld-gate-frames-replayed` | The same, gate to house: a withheld "closed" STATUS replayed later keeps the sensor closed and the link up while the gate is open | M | Open: house-only fix (accept STATUS only in seq order); TODO |
| `challenge-library-session-rebind` | The HELLO challenge is 32 bits and a HELLO_ACK isn't bound to the challenger's session, so answers farmed over weeks can re-verify a superseded peer session, whose recorded commands then replay | M | Open: protocol hardening (128-bit challenge bound to the session, no re-verifying an old session; ROADMAP) |
| `link-loss-concealment-window` | Jamming (or cutting the gate board's power) during an open–pass–close cycle hides it: the sensor stays closed until the link timeout, 100 s by default | H | Open: a "moved since you last heard" latch in STATUS, reported late rather than never (ROADMAP, with fault reporting) |
| `jamming-hides-state-until-link-timeout` | Jamming freezes the reported state for up to the link timeout and drops commands | H | Accepted, bounded by the timeout. Shorten `heartbeat_s`/`link_timeout_s` at the install if the window matters; report link loss separately (ROADMAP) |
| `cleartext-traffic-analysis` | Listeners learn gate state, AC loss, command times, reboots, firmware version | M | Accepted for now. Payload encryption (encrypt-then-MAC, per-direction keys) is on the ROADMAP with the protocol hardening |
| `session-id-repeat` | A session id repeating under one key (birthday bound, flash replaced or rolled back) lets old frames replay | M | Partly open: unique-by-construction ids from the boot counter (PATCH) and the boot counter's torn-slot bug (TODO); per-session MAC keys with the protocol hardening (ROADMAP) |
| `replayed-hello-side-effects` | Replayed HELLOs can delay a held command until it expires | L | Accepted (availability only; the house resyncs the switch) |
| `replay-flood-erases-evidence-and-reflects` | Floods overwrite the 64-entry log; memo'd seqs are re-ACKed without a limit | L | Open: coalesce flood events, persistent history (ROADMAP) |
| `pre-auth-parsing-surface` | Malformed frames reaching parsers | L | Mitigated: MAC before parse; fuzzed (`tests/native/fuzz`) |
| `reflection-direction-confusion`, `protocol-downgrade`, `net-id-sync-word-not-secret` | Reflecting frames, falling back to older formats, relying on net_id or the sync word | L | Mitigated. net_id and the sync word are filters against other users, not security |

### Availability and safety at the site

| Id | Threat | Sev | Status |
|---|---|---|---|
| `upstream-controller-trusted` | Anyone who can switch the Shelly (Z-Wave, Alarm.com account, automations) opens the gate: GateLink trusts every switch edge | H | Accepted by design: install checklist (Z-Wave S2 Authenticated, Alarm.com 2FA, least privilege, review automations) |
| `closed-limit-nc-fail-unsafe` | The closed limit comes from the AUX2 relay's NC contact: an opener-side AUX failure while its 24 V is present reads "closed" | H | Open: install and annual check (gate part-open and on battery: IN2 must read off); plausibility checks in firmware (ROADMAP) |
| `ac-cut-refuses-close` | Cutting AC (or the IN3 sense) stops remote closing, so the gate can be kept open | M | **Decision needed**: allow CLOSE without AC while a limit reads (ROADMAP) |
| `shelly-reboot-spurious-commands` | A Shelly reboot with power present can produce a CLOSE then an OPEN | M | Open: Shelly settings at install; detection (ROADMAP) |
| `power-sense-degradation-silent` | A failing IN2/IN3 opto or wire quietly disables remote control | M | Open: flag implausible power-sense readings (ROADMAP) |
| `reboot-loops` | Reset loops (brown-out, stuck I2C) leave a board alive but useless, silently | M | Open: count boot loops, report gate reboots to the house (ROADMAP) |
| `relay-held-welded-or-overlapping` | A relay held too long, both on, or a welded contact on the shared opener inputs | M | Mitigated in firmware (pulse-only, interlock, nothing blocks the loop mid-pulse; host-tested). A weld is invisible: sense the terminal on IN4 (ROADMAP) |
| `shared-opener-inputs` | The AES, the siren and local controls move the gate outside GateLink | M | Accepted: GateLink reports these moves (cause external) but can't prevent them |
| `output-fail-directions` | How each output fails | L | Mitigated. `sensor_invert` 1 makes the sensor fail closed: avoid it |

### Physical and console access

Physical or USB access to either board is full control of that board **and compromise of the link key** (the key is
in plain text in the external flash, the SAMD21's bootloader accepts any image, and SWD is open). No firmware change
removes this on this hardware; the controls are physical security, tamper detection and rotating the key afterwards.

| Id | Threat | Sev | Status |
|---|---|---|---|
| `key-extract-physical`, `key-extraction-reflash-swd`, `external-nor-direct-read` | The key read off a board in minutes (a sketch over USB, SWD, or a clip on the flash chip), giving lasting remote control | H | Accepted: locked, tamper-evident enclosures; rotate after any access ([key-management.md](key-management.md)); tamper switch on gate IN4 (ROADMAP) |
| `malicious-firmware-reflash` | Any image can be flashed with a 1200-baud touch or a double-tap of reset | H | Accepted (no secure boot on the stock bootloader); record `fw` and `flash_id` at commissioning and recheck on visits |
| `no-console-authentication-root`, `direct-relay-actuation-usb`, `alarm-sensor-tamper-config` | Anyone at a USB port can pulse the gate, change safety settings or the key | H | Open: an install-time console lock (state-changing commands need the key) (ROADMAP). Until then: nothing plugged in, boxes locked |
| `board-leaves-custody` | A lost, stolen, retired or returned board keeps a working key | M | Open: lifecycle rule (new board means new key; verified wipe), in [key-management.md](key-management.md) |
| `uart-console-left-enabled`, `debug-commands-in-production`, `unauthenticated-rekey` | Wider console surface | L–M | Install checklist: `uart_console` 0, nothing on USB |

### Keys

| Id | Threat | Sev | Status |
|---|---|---|---|
| `weak-or-shared-key` | key.set accepted all-zero and other trivial keys; the bench key could end up in the field | H | **Fixed** by the key lifecycle work: weak keys refused; field key separate from the bench key |
| `plaintext-key-copies` | Copies in `~/.gatelink_key`, environment variables, the clipboard | M | **Fixed** for backups: one encrypted backup format (GLKB v1) in the page and the tools; the bench file stays a bench convenience |
| `key-lifecycle-unverifiable` | No way to tell which key a board holds; rotation and restore fail silently | M | **Fixed**: boards report a key id (a 32-bit HMAC of the key) |
| `keygen-in-hosted-page` | Keys generated in a page that can also flash firmware | H | Mitigated: generate field keys offline (`tools/gatelink.py key gen`), or from a local checkout |
| `old-key-in-older-record` | After key.set the old key stays in the older flash sector until the next save | L | Open (TODO) |
| `boot-counter-torn-slot` | A torn boot-counter slot sticks the counter at 1 (found by fuzzing) | L | Open (TODO) |
| `rng-health`, `crypto-wearout` | Entropy health; years under one key | L | Accepted; rotation is driven by events, not age |

### Supply chain and the web console

The firmware and the console reach a board through GitHub. A compromised account, workflow, dependency or Pages
deploy could flash a backdoored image or take the key through the page. Nothing on the board would notice.

| Id | Threat | Sev | Status |
|---|---|---|---|
| `pages-malicious-firmware` | A compromised repo or deploy serves a console that flashes a backdoor or steals the key | H | Open: repository rulesets (PRs and CI required on `main`, protected `v*` tags), deploy Pages only after CI passes, immutable releases, build provenance attestations. **Repository settings are the owner's decision** |
| `actions-tag-pinning` | Workflows use actions by tag or branch (`setup-arduino-cli@v2` is a branch), and arduino-cli is unpinned | M | Open: pin actions by SHA, pin arduino-cli, split the release job's write token (CI hardening) |
| `release-asset-mutable` | A release binary can be replaced, and its `.sha256` shares the same trust root | M | Open: immutable releases, attestations; Pages fails closed without a checksum |
| `shared-github-io-origin` | The console shares `ccirone2.github.io` (and its Web Serial grants) with the account's other Pages sites | M | **Decision needed**: serve it from its own origin (custom domain or organisation). Until then, Forget the ports after an install visit |
| `arduino-deps-integrity` | Core and libraries pinned by version, not hash | M | Open: hash lock checked in CI; the dependency routine proposes updates as PRs |
| `agent-tooling-push-path` | Hooks and scheduled agents as a path into `main` | M | Mitigated in part: routines open PRs only; branch rules would enforce it |
| `tamper-undetectable` | No independent check that a board runs a genuine image | L | Open: read the image back and compare its hash (ROADMAP) |
| `board-output-xss`, `local-bin-forged-marker`, `npm-dev-deps`, `python-deps-unpinned`, `ha-token-tls-off`, `bench-wiring-server`, `ci-event-isolation` | Smaller web and bench hygiene items | L | Mostly open (TODO); `ci-event-isolation` handled well |

## Accepted risks

- **Physical access is game over** for the board reached: keep both boxes locked and tamper-evident, and treat any
  sign of access as key compromise.
- **Jamming wins availability.** GateLink fails safe (sensor open after the link timeout) and records it; it can't
  keep the link up.
- **The controller is trusted.** Whoever controls the Shelly controls the gate; secure the Z-Wave network and the
  Alarm.com account.
- **The opener's other inputs** are outside GateLink.
- **Traffic is readable** until payload encryption lands.

## Install checklist (security)

To be folded into the install checklist ([TODO.md](../TODO.md), "Install checklist"):

1. Generate a **field key** offline. It must differ from the bench key. Back it up encrypted
   ([key-management.md](key-management.md)). Set it on both boards, then check both report the same key id.
2. Lock both enclosures and make them tamper-evident. Leave nothing plugged into USB. Keep `uart_console` 0, and
   the UART pins unwired.
3. Keep `linkloss_open` 1 and `sensor_invert` 0, and every `inN_invert` 0.
4. Shelly: include it with Z-Wave S2 Authenticated, and check its power-failure restore and OTA settings. Alarm.com:
   2FA and least privilege for every user, and review the automations that touch the gate switch.
5. Check the limits: with the gate part-open, and with AC off on battery, IN2 reads off.
6. Record each board's USB serial, `fw`, `flash_id` and key id. Recheck them on every service visit.
7. After the install, have the service laptop's browser Forget the ports.

## Keeping this current

Update this page with any change to the frame format, the console's commands, key handling, the release or deploy
workflows, or the install procedure, and when a threat above is fixed. The ids are stable: refer to them in pull
requests, TODO.md and ROADMAP.md.
