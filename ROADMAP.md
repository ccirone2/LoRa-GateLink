# Roadmap

Desired features and ideas. Bugs and test tasks go in [TODO.md](TODO.md); shipped features are in the
[release notes](https://github.com/ccirone2/LoRa-GateLink/releases).

Each item: a bold title, why it's wanted, and notes or open questions. Move an item up to **Next** when it's
decided, and remove it once it ships (the pull request and release notes describe it). Anything touching gate
behaviour must keep the behavioural invariants in [CLAUDE.md](CLAUDE.md).

## Next

- **Link history in the web console.** The boards keep a link quality history since 0.4.0 (`hist.get`, see
  [docs/console.md](docs/console.md#link-history); `tools/gatelink.py <board> hist` gives it as CSV). Still to
  do: a "Link history" card on the Tools tab that fetches every page and charts it over wall time (bucket `i`
  started `now_s − i × period_s` seconds before the reply):
  - the RSSI min–avg band here and at the peer;
  - SNR margin (SNR minus the demodulation floor for the current `sf`: −7.5 dB at SF7 … −20 dB at SF12);
  - the noise floor;
  - a strip of problem marks (giveups, CRC errors, link-down minutes).
  Above the chart, a summary: availability %, worst SNR margin, retry % (retries / tx), hours with giveups. Add
  CSV export, and use the generic "controller" naming as everywhere in `web/`. No firmware change. Open
  questions about the history itself:
  - Should it survive resets? The linker script has no `.noinit` section, so any reset wipes the ring.
    Persisting it hourly to the SPI flash would pair with "Keep config and key across firmware uploads".
  - Is four days deep enough? RAM limits it: 96 hourly buckets of 64 B, as a `config.get` reply needs ~5 KB of
    heap (`free_ram` in status). A slimmer bucket, or 2 h buckets, would give a week.
  - Should the gate's own history ever be fetchable over LoRa (paged, like DIAG), or are the counters in
    STATUS enough?

## Ideas

- **Keep config and key across firmware uploads.** Every upload erases program flash, so role, key and
  `tx_power` must be re-applied after each flash (`tools/gatelink.py snapshot`/`restore` papers over it on the
  bench). The MKR WAN 1310 has a 2 MB SPI flash chip that uploads don't touch; storing `Config` there (with the
  existing CRC and `CFG_VERSION` check, migrating or falling back to defaults on a version change) would make
  field updates much safer. Open question: should a version change keep the key even when the rest resets?
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
  warn before it runs flat.
- **Flash firmware from the web console.** The SAMD bootloader (BOSSA) can be driven over Web Serial, so a field
  update wouldn't need `arduino-cli`. Pairs well with keeping config across uploads, and with the `.bin` attached
  to each release.
- **Key backup in config export.** The exported config leaves out the key, so restoring a board needs the key
  from elsewhere. An export option that includes the key encrypted with a passphrase would keep one backup file
  complete without putting the key in the clear.
