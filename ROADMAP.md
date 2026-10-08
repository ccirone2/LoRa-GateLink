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
    uses sectors 0–1). The linker
    script has no `.noinit` section, so even a watchdog reset clears RAM.
  - Go deeper? A week needs a slimmer bucket (64 B now) or 2 h buckets: a `config.get` reply takes ~5 KB of
    heap (`free_ram` in status).
  - Should the gate's own history be fetchable over LoRa (paged, like DIAG), or are the counters in STATUS
    enough?
- **Key backup in config export.** The exported config leaves out the key, so restoring a board needs the key
  from elsewhere. An export option that includes the key encrypted with a passphrase would keep one backup file
  complete without putting the key in the clear.
