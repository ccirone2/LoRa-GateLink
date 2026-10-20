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
  payload is decrypted or parsed. Without the key no command, state or config write can be forged (`frame-forgery`:
  low).
- **Payloads are private** (0.16.0). They are encrypted (AES-128-CTR under a key derived from the link key,
  encrypt-then-MAC): a listener sees message types, lengths and times, not the gate's state, the AC, the command or
  any setting (`cleartext-traffic-analysis`: low).
- **No replay of frames that were already delivered.** Each boot draws a fresh session id (unique by construction
  from the boot counter while it works, 0.13.9) and a random starting seq. A peer's session counts only once it has echoed a fresh challenge, and from then on only seqs above that,
  each once (32-frame window). Delivered frames can't be replayed, within a session or across reboots
  (`recorded-frame-replay`: low).
  - Frames **withheld** (jammed at the receiver and recorded) and delivered later aren't stopped by the window, so
    the messages that matter are checked for it: a withheld gate STATUS changes nothing (0.13.9), and a withheld
    command is past its deadline in the gate's clock (0.16.0). Challenges are 128 bits (0.16.0), so a library of
    recorded answers never matches one.
- **No late commands** (0.16.0). Every command carries a deadline in the gate's own clock (when the house sent it
  plus `cmd_ttl_s`, which the house reckons from STATUS); the gate refuses one past it by more than 2 s plus a
  STATUS's and a command's time on the air, so no command moves the gate later than that, however it was delayed
  (`withheld-command-fires-late`: low).
- **No duplicate pulses** for one command, whatever is lost or restarted. The gate de-duplicates by command id, and
  a command pending across a gate restart is dropped, not resent.
- **Fail-safe outputs.** Relays drop on any reset. The gate's inputs are pulled down, so a dead opto or cut wire
  never reads as a limit. The house never commands at boot. The contact sensor fails open when the link is lost:
  after max(`link_timeout_s`, 2.5 × the gate's `heartbeat_s`), 100 s at the defaults.
- **The key can't be read back** over the console or the link, and the link stays off until a key is set.

It does **not** guarantee:

- **Hiding when things happen.** Message types and frame lengths are in the clear, and STATUS goes on every change:
  a listener can tell that a command went and that the gate's state changed, not which (`cleartext-traffic-analysis`).
- **Availability under jamming.** It can only fail safe and make the jamming visible (`rf-jamming-dos`,
  `jamming-hides-state-until-link-timeout`).
- **Anything against someone with access to a board.**

## Threats

Severity is after verification: **H**igh, **M**edium, **L**ow. Status says where each stands; open items link to
[TODO.md](../TODO.md) (bugs and tasks) or [ROADMAP.md](../ROADMAP.md) (features and decisions).

### Radio link

| Id | Threat | Sev | Status |
|---|---|---|---|
| `hello-answer-stamp-deadlock` | After ~24.9 days of steady link, a peer restart's HELLOs were ignored for up to another ~24.9 days (the HELLO answer stamps read as recent under the signed `elapsed()`): link down, sensor open | H | **Fixed** in 0.13.7, with host tests past 25 days (#84) |
| `backoff-starvation-by-frequent-frames` | Any LoRa frame heard every ~100 ms held all new frames off until their TTL: a cheap, silent DoS | M | **Fixed** in 0.13.7 (link PR) |
| `jam-and-delay-replay-window` / `withheld-command-fires-late` | A command jammed at the gate and recorded could be replayed later, while still inside the 32-frame window (about 16 minutes of normal traffic, or as long as the gate is kept from hearing the house): the gate opened at a time the attacker picked, after the house had dropped the command. The receiver can't tell a delayed frame from a retry | H | **Fixed** in 0.16.0: a command carries a deadline in the gate's 64-bit clock (from STATUS) and is refused more than 2 s (plus two frames' airtime) past it (`RES_LATE`, log `cmd_late`), host-tested to 2^32 ms. Each boot's clock starts from a random origin, so a deadline a house dated in the gate's previous boot (kept from seeing the restart: found in the security review) is refused too. A remote config write carries none (`withheld-config-write`) |
| `withheld-config-write` | A remote config write jammed at the gate and recorded, played back later inside the window, sets a value the installer chose before (after the console reported it failed): it can undo a later write | L | Accepted (0.16.0): only installer-chosen values, rarely written; recheck the gate's settings (`remote.diag`) after a write that failed |
| `withheld-gate-frames-replayed` | The same, gate to house: a withheld "closed" STATUS replayed later keeps the sensor closed and the link up while the gate is open | M | **Fixed** in 0.13.9 (#86): the house takes a STATUS only in seq order and its link stays up on STATUS alone. A relay that delays every STATUS, in order, can still hold the house behind by up to its link timeout (`link-loss-concealment-window`) |
| `challenge-library-session-rebind` | The HELLO challenge was 32 bits, so answers farmed over weeks (stale HELLOs replayed to make a board challenge, the live peer's answers recorded) could re-verify a superseded peer session, whose recorded frames (STATUS, commands) then replayed | M | **Fixed** in 0.16.0: 128-bit challenges, the link key's HMAC of the session, a count and radio noise. Binding the answer to the challenger's session and refusing a superseded session would add nothing with 128 bits: not done |
| `link-loss-concealment-window` | Jamming (or cutting the gate board's power) during an open–pass–close cycle hides it: the sensor stays closed until the link timeout, 100 s by default | H | Mitigated in 0.17.0: STATUS counts the gate's departures from closed, and a house that sees the count advance while its sensor showed closed opens the sensor for 5 s (log `missed_move`), so the alarm records the move late rather than never. Left: cutting the gate board's power loses the count (the house logs a new gate session); the alarm learns of the move only when a report gets through |
| `jamming-hides-state-until-link-timeout` | Jamming freezes the reported state for up to the link timeout and drops commands | H | Accepted, bounded by the timeout. Shorten `heartbeat_s`/`link_timeout_s` at the install if the window matters. Since 0.14.0 the fault output (D5, `fault_out`) reports link loss to the alarm apart from "open"; telling it apart from other problems is on the ROADMAP (F3) |
| `cleartext-traffic-analysis` | Listeners learned gate state, AC loss, commands, settings, reboots, firmware version | L | Mitigated in 0.16.0: payloads encrypted (AES-128-CTR, encrypt-then-MAC, the counter from `src`, `dst`, session and seq). Left: message types, lengths and times (a command went, the state changed) and reboots (HELLOs); padding to one length wouldn't hide them while STATUS goes on every change |
| `session-id-repeat` | A session id repeating under one key (birthday bound, flash replaced or rolled back) lets old frames replay, and would reuse a keystream | M | Partly: unique by construction in 0.13.9 (#86), a keyed permutation of the boot count, the draws this boot and 8 random bits, so a rolled-back count repeats an id 1 time in 256. Rolling the flash back needs a board in hand, which gives the key anyway, so per-session keys (once proposed with the protocol hardening) weren't built (0.16.0) |
| `replayed-hello-side-effects` | Replayed HELLOs can delay a held command until it expires | L | Accepted (availability only; the house resyncs the switch) |
| `replay-flood-erases-evidence-and-reflects` | Floods overwrite the 64-entry log; memo'd seqs are re-ACKed without a limit | L | Partly: the hourly link history is kept in flash since 0.14.0, with MAC failures but not replays. Since 0.19.0 the event log coalesces a flood (`mac_fail`, `replay`, `lbt_forced`) into one entry with its count, keeps its ring through every reset but power-on, and writes it to a flash journal at most once a minute, so a flood neither erases the evidence nor keeps the radio busy writing. Open: count replays in the history, limit re-ACKs (ROADMAP F21) |
| `pre-auth-parsing-surface` | Malformed frames reaching parsers | L | Mitigated: MAC before parse; fuzzed (`tests/native/fuzz`) |
| `reflection-direction-confusion`, `protocol-downgrade`, `net-id-sync-word-not-secret` | Reflecting frames, falling back to older formats, relying on net_id or the sync word | L | Mitigated. net_id and the sync word are filters against other users, not security |

### Availability and safety at the site

| Id | Threat | Sev | Status |
|---|---|---|---|
| `upstream-controller-trusted` | Anyone who can switch the Shelly (Z-Wave, Alarm.com account, automations) opens the gate: GateLink trusts every switch edge | H | Accepted by design: install checklist (Z-Wave S2 Authenticated, Alarm.com 2FA, least privilege, review automations) |
| `closed-limit-nc-fail-unsafe` | The closed limit comes from the AUX2 relay's NC contact: an opener-side AUX failure while its 24 V is present reads "closed" | H | Partly (0.17.1): both limits reading at once (for 500 ms) holds the gate at `fault` (not closed) until the open limit reads alone, so a stuck closed limit reads not closed from the gate's first full opening on, where before it read closed everywhere but the open limit. From 0.17.2 an opener restart's dropped AUX relays don't read closed either: closed straight from open reads `fault`, and closed out of `no_power` waits 3 s. Left: the opener's own 24 V dropping while IN3 stays up (TODO.md); until that opening (and after a deliberate restart or a power-on of the gate board; since 0.17.4 a crash reset keeps the hold) it can't be told from a closed gate: install and annual check (gate part-open and on battery: IN2 must read off); an independent closed sensor (ROADMAP F12) |
| `ac-cut-refuses-close` | Cutting AC (or the IN3 sense) stops remote closing, so the gate can be kept open | M | Mitigated (0.15.1; OPEN too from 0.15.2): without AC, commands still run while a limit reads; with no limit reading (`no_power`), or both (`fault`, held since 0.17.1 until the open limit reads alone), they're refused. Left: a gate stopped part-way when the AC goes (or a battery too weak to finish the travel, which an OPEN on a long-drained battery can now cause) reads `no_power`: once our target has lapsed CLOSE is refused, and the house shows not-closed (and D5 drops). ROADMAP F4 (opener battery warning) narrows it |
| `shelly-reboot-spurious-commands` | A Shelly reboot with power present can produce a CLOSE then an OPEN | M | Open: Shelly settings at install; detection (ROADMAP F16) |
| `power-sense-degradation-silent` | A failing IN2/IN3 opto or wire quietly disables remote control | M | Gate IN3: not silent (a dead IN3 reads as AC lost: health `ac` and D5 since 0.14.0, STATUS bit 6; commands still run while a limit reads since 0.15.2). House IN2: mitigated (0.17.3): a dead opto reads as a controller without power, so switch edges are ignored, and IN2 off for 30 s while the board's own supply (the same 12 V) is good is health `ctrl_sense` (D5). Needs both power senses on and the board fed from the controller's supply |
| `reboot-loops` | Reset loops (brown-out, stuck I2C) leave a board alive but useless, silently | M | Mitigated (0.15.0): three watchdog or brown-out resets in a row, each within an hour of the last, are health `reboots` on that board and its D5 until it has run an hour; the gate reports its loop in STATUS (bit 7) and the house raises `reboots` too. After a watchdog reset the board reports where it hung (`hang_stage`, `hang_pc`). Not counted: loops of full power-on resets (a gate one shows at the house as a link that keeps dropping). Open: telling the alarm which trouble it is (ROADMAP F3) |
| `relay-held-welded-or-overlapping` | A relay held too long, both on, or a welded contact on the shared opener inputs | M | Mitigated in firmware (pulse-only, interlock, nothing blocks the loop mid-pulse; host-tested). A weld is invisible: sense the terminal on an input (ROADMAP F14) |
| `shared-opener-inputs` | The AES, the siren and local controls move the gate outside GateLink | M | Accepted: GateLink reports these moves (cause external) but can't prevent them |
| `output-fail-directions` | How each output fails | L | Mitigated. `sensor_invert` 1 makes the sensor fail closed: avoid it |

### Physical and console access

Physical or USB access to either board is full control of that board **and compromise of the link key** (the key is
in plain text in the external flash, the SAMD21's bootloader accepts any image, and SWD is open). No firmware change
removes this on this hardware; the controls are physical security, tamper detection and rotating the key afterwards.

| Id | Threat | Sev | Status |
|---|---|---|---|
| `key-extract-physical`, `key-extraction-reflash-swd`, `external-nor-direct-read` | The key read off a board in minutes (a sketch over USB, SWD, or a clip on the flash chip), giving lasting remote control | H | Accepted: locked, tamper-evident enclosures; rotate after any access ([key-management.md](key-management.md)); tamper switch (ROADMAP F19); a board with secure boot and flash encryption would stop it (ROADMAP H2) |
| `malicious-firmware-reflash` | Any image can be flashed with a 1200-baud touch or a double-tap of reset | H | Accepted (no secure boot on the stock bootloader; a board with it: ROADMAP H2); record `fw` and `flash_id` at commissioning and recheck on visits |
| `no-console-authentication-root`, `direct-relay-actuation-usb`, `alarm-sensor-tamper-config` | Anyone at a USB port can pulse the gate, change safety settings or the key | H | Open: an install-time console lock (state-changing commands need the key) (ROADMAP F18). Until then: nothing plugged in, boxes locked |
| `board-leaves-custody` | A lost, stolen, retired or returned board keeps a working key | M | Open: lifecycle rule (new board means new key; verified wipe), in [key-management.md](key-management.md) |
| `uart-console-left-enabled`, `debug-commands-in-production`, `unauthenticated-rekey` | Wider console surface | L–M | Install checklist: `uart_console` 0, nothing on USB |

### Keys

| Id | Threat | Sev | Status |
|---|---|---|---|
| `weak-or-shared-key` | key.set accepted all-zero and other trivial keys; the bench key could end up in the field | H | **Fixed** by the key lifecycle work: weak keys refused; field key separate from the bench key |
| `plaintext-key-copies` | Copies in `~/.gatelink_key`, environment variables, the clipboard | M | **Fixed** for backups: one encrypted backup format (GLKB v1) in the page and the tools; the bench file stays a bench convenience |
| `key-lifecycle-unverifiable` | No way to tell which key a board holds; rotation and restore fail silently | M | **Fixed**: boards report a key id (a 32-bit HMAC of the key) |
| `keygen-in-hosted-page` | Keys generated in a page that can also flash firmware | H | Mitigated: generate field keys offline (`tools/gatelink.py key gen`), or from a local checkout |
| `old-key-in-older-record` | After key.set the old key stayed in the older flash sector until the next save | L | **Fixed** in 0.13.9 (#86): zeroed once the new record verifies |
| `boot-counter-torn-slot` | A torn boot-counter slot stuck the counter at 1 (found by fuzzing) | L | **Fixed** in 0.13.9 (#86): two-step slot writes; a garbled read gives no count |
| `rng-health`, `crypto-wearout` | Entropy health; years under one key | L | Accepted; rotation is driven by events, not age |

### Supply chain and the web console

The firmware and the console reach a board through GitHub. A compromised account, workflow, dependency or Pages
deploy could flash a backdoored image or take the key through the page. Nothing on the board would notice.

| Id | Threat | Sev | Status |
|---|---|---|---|
| `pages-malicious-firmware` | A compromised repo or deploy serves a console that flashes a backdoor or steals the key | H | Partly: Pages deploys only after CI passes on `main` (#74). Since 2026-10-14 rulesets require a pull request with CI's six jobs passing for `main` (no force push or deletion) and keep `v*` tags from moving or being deleted, and releases are immutable from v0.17.0 (GitHub attests the tag, commit and files: `gh release verify`). Left: the rulesets can't tell the owner's sessions and routines apart (one account); build provenance for the binary. **Repository settings are the owner's decision** |
| `actions-tag-pinning` | Workflows use actions by tag or branch (`setup-arduino-cli@v2` is a branch), and arduino-cli is unpinned | M | Partly: actions pinned by commit SHA, with Dependabot proposing updates (#74); cloud sessions install arduino-cli by checksum, and every workflow pins its version (a test checks; unpinned, CI had taken a release candidate). The release workflow's write token is only in jobs that run GitHub's own API and actions; the build has a read-only one. Open: CI checks arduino-cli's checksum too (with `arduino-deps-integrity`) |
| `release-asset-mutable` | A release binary can be replaced, and its `.sha256` shares the same trust root | M | Mitigated from v0.17.0: releases are immutable (files and tag locked at publish; the workflow attaches everything to a draft first) and GitHub signs a release attestation (`gh release verify-asset`). Left: releases before v0.17.0 stay mutable; Pages checks the bundled `.bin` against the `.sha256` only |
| `shared-github-io-origin` | The console shares `ccirone2.github.io` (and its Web Serial grants) with the account's other Pages sites | M | Deferred (2026-10-12): serve it from its own origin (custom domain or organisation), maybe eventually (ROADMAP C5). Until then, Forget the ports after an install visit |
| `arduino-deps-integrity` | Core and libraries pinned by version, not hash | M | Open: hash lock checked in CI; the dependency routine proposes updates as PRs |
| `agent-tooling-push-path` | Hooks and scheduled agents as a path into `main` | M | Mitigated in part: routines open PRs only, and `main` takes changes only through a pull request with CI passing (ruleset). A ruleset can't tell a routine from the owner (they act as one account), no review is required, and a pull request runs its own CI: the ruleset catches mistakes, and "routines never merge or release" rests on their instructions |
| `tamper-undetectable` | No independent check that a board runs a genuine image | L | Open: read the image back and compare its hash (ROADMAP C4) |
| `board-output-xss`, `local-bin-forged-marker`, `npm-dev-deps`, `python-deps-unpinned`, `ha-token-tls-off`, `bench-wiring-server`, `ci-event-isolation` | Smaller web and bench hygiene items | L | Mostly fixed; `ci-event-isolation` handled well; `python-deps-unpinned` fixed (`uv.lock`, with hashes, used by CI and the bench); `board-output-xss` fixed (a content security policy, `hist.get` values taken as numbers only); `ha-token-tls-off` fixed (HA's certificate always verified: system CAs or `GATELINK_HA_CA`); `bench-wiring-server` fixed (localhost Host header only); `local-bin-forged-marker`, `npm-dev-deps` open |

## Accepted risks

- **Physical access is game over** for the board reached: keep both boxes locked and tamper-evident, and treat any
  sign of access as key compromise.
- **Jamming wins availability.** GateLink fails safe (sensor open after the link timeout) and records it; it can't
  keep the link up.
- **The controller is trusted.** Whoever controls the Shelly controls the gate; secure the Z-Wave network and the
  Alarm.com account.
- **The opener's other inputs** are outside GateLink.
- **When things happen is visible.** Payloads are encrypted, but types, lengths and times aren't.

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
