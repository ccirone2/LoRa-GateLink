# Proposal: an M5Stack hardware architecture for GateLink

Status: proposal (2026-10-30), not decided. It answers ROADMAP [H2](../../ROADMAP.md) ("cheaper boards that protect
the key") with a concrete, off-the-shelf design. Prices are reseller figures from October 2026 and will drift.

## Summary

Replace the MKR WAN 1310 + MKR Relay Proto Shield + opto board + buck + fault relay module at each end with **one
M5Stack StamPLC (K141)** and **one M5Stack Unit LoRaWAN-US915 (U184)** on a Grove cable:

| | Today (MKR) | Proposed (M5Stack) |
|---|---|---|
| Controller | SAMD21, 32 KB RAM, key in plain-text SPI flash, any image flashable | ESP32-S3 (8 MB flash), flash encryption + Secure Boot v2 protect the key |
| Inputs | 4 × 3.3 V pins through an external PNP opto board (3.3 V only) | 8 × opto-isolated inputs, DC 5–36 V, 24 V wetted contacts wired straight in |
| Outputs | 2 relays (3 V coils on the 3.3 V rail) + D5 logic output needing a relay module | 4 relays, 5 A: K1, K2, fault loop, one spare |
| Power | 5 V max VIN: a 24 V→5 V buck at the gate, a 12 V→5 V buck at the house | DC 6–36 V barrel jack: the opener's 24 V and the controller's 12 V go straight in |
| Mounting | bare boards in a box | DIN-rail case with screw terminals, display, three buttons, RTC, microSD |
| Radio | Murata CMWX1ZZABZ (SX1276) on SPI, our own driver | RAK3172 (STM32WL, SX126x core) in P2P mode over UART AT commands |
| Cost per location | ~$95–115 | ~$75–90 |

The gate and house boards stay one firmware with the role in config; the frame format, `link.cpp` and both state
machines do not change. What changes is everything behind `board.h`, `radio.cpp`, `extflash.cpp`/`config.cpp`,
`supply.cpp`, `console_io.cpp`, `pins.h`, and the web console's flasher. ROADMAP F11 (everything chip-specific behind
`board.h`) is the prerequisite, and is worth doing first whatever the decision.

## Why M5Stack, and why this M5Stack

The goals rank Truth, Availability, Safety, then Diagnosability, and count complexity and cost. Against them the MKR
stack has four standing costs: the key is readable by anyone who opens the box (threat model `key-extract-physical`,
`malicious-firmware-reflash`, accepted today only because the SAMD21 can't help); the 3.3 V-only inputs need an
external opto board and careful wiring notes; the 5 V VIN limit needs a buck at each end; the fault output (D5) needs
yet another relay module. Each is a part the installer wires and a page of `docs/hardware.md`.

The StamPLC is the one M5Stack product that removes all four at once: isolated 24 V inputs, 5 A relays, 6–36 V in,
DIN rail, and an ESP32-S3 whose eFuse-held keys only the hardware can read. The other M5Stack routes were checked and
don't fit:

- Core/CoreS3 + Module LoRa868 v1.2 (E22-900M22S, SX1262, 850–930 MHz) + 4Relay Module: the relay module is 24 V /
  1 A DC only (fine for the opener's dry inputs, but no isolated inputs anywhere in the stack), ~$110 before an
  enclosure, and the Module LoRa868 v1.1/v1.2 are end-of-life at several resellers.
- Atom DTU LoRaWAN (A152) / Atomic DTU base: radio plus RS485 only, no relays or isolated inputs; it would need the
  StamPLC anyway.
- Unit C6L (ESP32-C6 + SX1262): a sealed Meshtastic node with a Grove port, no I/O; reflashable, but then it is a
  second MCU running our radio code with its own firmware update path.
- Unit 4-Relay (U097, I2C, 10 A): good relays, but the inputs would still need the opto board and a host.

## Parts per location

| Part | Role | Price (Oct 2026) |
|---|---|---|
| M5Stack StamPLC (K141) | controller, 8 isolated inputs, 4 relays, DIN rail, display | $42.90 (M5Stack shop launch price; DigiKey €36.74) |
| M5Stack Unit LoRaWAN-US915 (U184) | RAK3172 radio, 22 dBm, −137 dBm, SMA antenna included, FCC-certified module | ~$20–25 (not confirmed: the comparable Atomic DTU LoRaWAN base lists ~$23 at Mouser) |
| SMA extension or bulkhead | antenna outside the box (the unit's SMA is on the unit) | ~$5 |
| DIN enclosure / rail stub, terminals | both ends already have a box | $0–15 |
| **Total** | | **~$70–90** |

Today's bill for comparison: MKR WAN 1310 ~$46 (Arduino store; £38 ex-VAT at RS), Relay Proto Shield ~€17–21, opto
board ~$10, buck ~$5, fault relay module ~$3, LiPo ~$8, antenna pigtail ~$5: ~$95–115. The new design also drops the
LiPo (see "Power", a real trade-off) and the opto board's wiring notes.

Not needed: the StamPLC IO Module (A176: 2 analog inputs, 2 MOSFET outputs, 1 relay). The spare inputs IN5–IN8 and
relay 4 already cover ROADMAP F15 (beam-break, alarm status, "opener battery low" via a comparator or the IO module's
0–10 V input later).

## Wiring

All inputs are "high-level sensors" in M5Stack's terms: `EXCOM_COM` to the supply negative, each `EXCOM_INn` fed
+24 V (gate) or +12 V (house) through the contact. A lit opto reads active; a dead opener, cut wire or dead channel
reads off, exactly the fail-toward-not-closed property the pull-down inputs give today (`docs/hardware.md`). No
3.3 V anywhere at the terminals.

| StamPLC terminal | House | Gate |
|---|---|---|
| IN1 | Shelly relay output (O), I ← Shelly 12 V + | CSW24UL AUX1 "open limit" NO, wetted from the 24 V accessory output |
| IN2 | Shelly 12 V + (`ctrl_power_sense`) | CSW24UL AUX2 "not at close limit" **NC**, wetted from the accessory output |
| IN3 | spare | AC power sense: the mains 24 V supply (`power_sense`; DC only: the EL3H4 is a DC opto, rectify a 24 VAC supply) |
| IN4 | spare | Leaf closed sensor, wetted from the accessory output (`closed_sense`) |
| IN5–IN8 | spare | spare |
| COM | Shelly 12 V − | opener 24 V − |
| RLY1 | Shelly SW input (energized = gate not closed) | CSW24UL OPEN + COM (pulsed only) |
| RLY2 | 2GIG contact sensor terminals (energized = gate closed) | CSW24UL CLOSE + COM (pulsed only) |
| RLY3 | `fault_out` loop: NO/COM closed while healthy, to a second 2GIG sensor or zone | same, to a sensor near the opener |
| RLY4 | spare | spare |
| DC in | the Shelly's 12 V supply | the opener's 24 V accessory output (battery-backed) |
| PORT.A (Grove) | Unit LoRaWAN-US915 | Unit LoRaWAN-US915 |
| USB-C | web console | web console |

The CSW24UL AUX relay settings, the NC closed-limit reasoning and the `closed_sense` rules in `docs/hardware.md` carry
over unchanged: the StamPLC only replaces the opto board between those contacts and the MCU.

## Relays and inputs through an I2C expander: invariants check

On the StamPLC the relays and inputs are not MCU pins: an AW9523B expander (I2C 0x59, 400 kHz) drives a ULN2003A
low-side driver for the four HF3FF relay coils and reads the eight EL3H4 optos (M5Stack's StamPLC_IO schematic). That
changes nothing in the state machines but needs three things in the board layer:

- **Fail de-energized.** The AW9523B at address 0x59 has AD1=GND, AD0=VCC, and its datasheet's power-on table gives
  P0_0–P0_3 (the relay drives) as **Hi-Z** after power-on or `RSTN`; the ULN2003A inputs then float and the coils
  are off. So a reset, a brown-out or a hung MCU drops every relay, as the MKR shield does today. `RSTN` is G3,
  shared with the LCD: pulling it low during boot (before the watchdog) is the "relays de-energised first" step of
  `setup()`.
- **A pulse must end on time.** `Relay::update` releases a coil with one I2C write (~0.1 ms at 400 kHz): the
  `INTERLOCK_MS` and pulse timings are unaffected. The new failure mode is an I2C bus that hangs mid-pulse (a stuck
  slave, a glitch on the shared bus). Mitigation: a failed or NACKed release write asserts `RSTN` (G3) low for the
  datasheet's 10 µs and re-initialises the expander, which drops all four relays within the loop pass; log it as a
  `relay` fault. This is the relay-side counterpart of today's `radioRecover`. Host tests: the simulated board layer
  gets an "I2C hangs" fault injection and the invariant checker must see the coil released within one loop pass.
- **Inputs are read as one byte per port.** All eight inputs arrive in one I2C read per loop pass (the `INT` line on
  G14 is available but not needed at loop rate). `updateSpareInputs` before `readState()` in `gateLoop` stays as is;
  `inN_invert` stays at 0 and is still never remote-writable.

The display, buttons and buzzer (PI4IOE5V6408 on the same bus) are a diagnosability gain for the installer at the gate
(state, link, last fault, key id) and can show the same `status` fields the web console shows; nothing may decide on
them.

## Radio: the Unit LoRaWAN-US915 in P2P mode

The unit is a RAK3172 (STM32WLE5, an SX126x radio core) running RAK's RUI3 firmware, driven over the Grove UART at
115200 baud with AT commands. In `AT+NWM=0` (P2P LoRa) it does what `radio.cpp` does today, and the LoRa PHY is the
same, so the frame format and `link.cpp` are untouched:

| `radio.cpp` today (SX1276 registers) | RUI3 P2P |
|---|---|
| `freq_hz`, `sf`, `bw_hz`, coding rate, preamble, `tx_power` | `AT+P2P=915000000:9:500:1:8:22` (one command) |
| sync word 0x12 (private) | `AT+SYNCWORD=1424` (the SX126x encoding of 0x12) |
| CRC on | on in P2P mode |
| RX continuous, async TX with a deadline, never block the loop | `AT+PRECV=65533` (continuous RX that still allows TX); `AT+PSEND=<hex>` returns `OK` at once, `+EVT:TXP2P DONE` when the frame has gone: the same two-phase TX `radio.cpp` already models |
| Listen-before-talk from the modem status register | `AT+CAD=1`: the module runs channel activity detection before each send |
| RSSI, SNR per frame | in every `+EVT:RXP2P:<rssi>:<snr>:<hex>` |
| Frequency error (FEI) per frame, CRC-error and RX-done counters | **not available**: `fei` leaves the link history, bad-CRC frames are dropped silently |
| `LoRa.random()` for entropy | the ESP32-S3 hardware RNG (better) |
| Hold the modem in reset during a flash write | not needed: the ESP32-S3's flash is on its own bus, so saves no longer take the radio off the air |

Costs and risks, honestly:

- `radio.cpp` encodes hard-won SX1276 behaviour (ROADMAP H2 says so). Its replacement is a small AT state machine
  (~300 lines), and its failure modes move: a module that stops answering is recovered with `ATZ` (and, if the Grove
  5 V can't be switched, a watchdog reset of the whole board), like `radioRecover` today. The RUI3 firmware is a second
  firmware we don't control; pin its version in `docs/development.md` and record it in `status`.
- Latency: a 60-byte frame is 120 hex characters, ~10 ms at 115200 baud each way, on top of airtime; the link's
  `Slot` retries and `HELLO_INTERVAL_MS` (1 s) have far more margin than that. Measure the RX→TX turnaround on the
  bench before deciding (the `tests/e2e` ping run gives it).
- The payload is hex, 256 bytes max; GateLink frames are far smaller.
- RUI3's own P2P encryption (`AT+ENCRY`) stays **off**: the link's AES-CTR + HMAC is the security boundary and the key
  never leaves the ESP32-S3's encrypted flash.
- The unit's SMA is on the unit's case: a short SMA extension puts the antenna outside the DIN box, as the pigtail
  does today.

**Fallback (B):** a bare SX1262 module (an E22-900M22S or Waveshare Core1262, ~$10) on the StamPLC-Bus header, using
its SPI (G7/G8/G9, CS G11), G40/G41 for `BUSY`/`DIO1` and a PORT.C pin (G4) for `NRST` (G3 must not be used: it also
resets the relay expander and LCD). This keeps `radio.cpp`'s structure (ported to the SX1262 register/command set,
RadioLib is the obvious driver) and all its counters, at the price of soldering a header board, so it is not
"all M5Stack". Take it if the bench shows the AT route can't meet the turnaround or the CAD behaviour the link needs.

**Interoperability during migration.** An SX1276 (MKR) and an SX126x (RAK3172) talk to each other with matching
SF/BW/CR, the private sync word and LDRO settings, so one end can be swapped at a time and the frame format and key
stay. Two cautions from Semtech's forum: at the project's default 500 kHz bandwidth one report saw reduced sensitivity
between the two families with explicit headers, and SF5/SF6 are SX126x-only. Bench the mixed pair at SF9/500 kHz
first; if range suffers, migrate both ends in one visit (both boards need a gate visit anyway).

## Key protection: what the ESP32-S3 buys and what it costs

With flash encryption (XTS-AES-128, key in eFuse, read-protected) and Secure Boot v2 (RSA-3072 or ECDSA signature
checked by the ROM), in **Release mode**, the threat model's `key-extract-physical`, `key-extraction-reflash-swd`,
`external-nor-direct-read` and `malicious-firmware-reflash` move from Accepted to Mitigated: a clip on the flash
reads ciphertext, download mode won't decrypt or write plaintext, JTAG is disabled by eFuse, and only images signed
with the project key boot. Espressif documents the enable order (burn the Secure Boot key first, keep it readable;
then the flash-encryption key, unreadable). It is one-way per board.

What it costs the project:

1. **The web console can no longer flash an arbitrary image over USB.** Today `web/js/samba.js` drives the SAM-BA
   bootloader over Web Serial. On a Release-mode ESP32-S3, esptool-js (Web Serial, Chrome/Edge) can still write, but
   only an image pre-encrypted on the host for that board's key, which is a per-board lab step. The route that fits
   the goals: the running firmware accepts a **signed image over its own JSON console** (chunks into the OTA
   partition, which the chip encrypts on write; the ROM checks the signature at boot). That is ROADMAP F3's
   mechanism built for USB first and reused over LoRa later, and it closes `keygen-in-hosted-page`'s cousin (the page
   can't flash anything unsigned). First commissioning of a bare board (eFuses, bootloader, first image) is a
   developer step with esptool, recorded in `docs/key-management.md`.
2. The console stays unauthenticated at the USB port (ROADMAP F18 is still needed): the key is protected against
   reading, not against someone with USB access telling the board to pulse a relay or `key.set`.
3. Config and the link key move from the raw W25Q16 records (`extflash.cpp`) into an NVS or own partition on the
   encrypted flash; the two-slot torn-write rules in `config.cpp` carry over. `boardKeep()`'s `.noinit` words become
   RTC slow memory (kept across all resets but power-on, as today).

## Power

- **Gate:** the opener's 24 V accessory output straight into the barrel jack (6–36 V). No buck. The StamPLC's
  INA226 reads its own VIN: "opener battery low" (ROADMAP F15) becomes a reading instead of a comparator.
- **House:** the Shelly's 12 V straight in. The MKR design relies on its LiPo to stay up through a controller power
  cut and the BQ24195L's power-good bit to see the cut ~0.2 s before the Shelly's relay drops (`ctrl_power_pmic`).
  The StamPLC has no battery, so a cut reboots it. The invariants already make a reboot safe (no command before the
  first STATUS and a sync window, `armed`; `checkSoon` resync after power return), so Truth and Safety hold; the costs
  are a few seconds of link loss (the house's K2 fails open: a nuisance "not closed" blip at Alarm.com) and losing the
  300 ms-dip detection. Three options, in order of preference:
  1. Power the house StamPLC from its own always-on 12 V supply and keep IN2 on the Shelly's 12 V. The INA226 then
     reports the board's own supply, and the controller sense is IN2 alone: set `ctrl_confirm_ms` to ~3 s, above the
     opto's ~1.65 s lag, as `docs/hardware.md` already prescribes for `ctrl_power_pmic` off. Cheapest, and the alarm
     never sees a blip from a Shelly power cut.
  2. Share the 12 V and add a small 12 V DC UPS or supercap module (~$15–20): keeps today's behaviour, costs budget.
  3. Share the 12 V and accept the reboot.
  The INA226 is still useful: `ctrl_power_pmic` becomes "VIN below threshold", sampled every loop pass, which on
  option 2 sees a sag earlier than the charger's power-good bit did.
- The 6–36 V input and the TX current: the Unit's own figures are ~117 mA at 125 kbps during TX from the Grove 5 V
  (700 mA available), so the "full-power TX with a relay energised crashed the board on USB power" bench rule relaxes
  but should be re-measured, as should everything in `docs/bench-testing.md` that quotes MKR numbers.

## Firmware and tooling changes, by layer

| Layer | Change |
|---|---|
| F11 first | move the remaining chip touches (`PM_RCAUSE_*` in `app.cpp`, `.noinit`, `FlashStorage`, `Wire` in `supply.cpp`, pin reads/writes in `io.cpp`, USB in `console_io.cpp`) behind `board.h`; host tests unchanged |
| `board.h` (ESP32-S3) | watchdog (task WDT or RTC WDT, 8 s), reset cause, RTC memory for `boardKeep`, free RAM, relays and inputs through the AW9523B with the reset-on-fault above |
| `radio.cpp` | RUI3 AT backend (or SX1262 SPI, fallback B); `radio_ok`, faults, RSSI/SNR kept; `fei`, CRC counters dropped from `status`/history (a MINOR, both boards: update `roles.h`, `docs/console.md`, `tests/web/fake-serial.js`, `tools/gatelink_client/`, `tools/check_contract.py`) |
| `extflash.cpp` / `config.cpp` | encrypted-flash partition backend; same record format and torn-write tests; saves no longer silence the radio |
| `supply.cpp` | INA226 instead of BQ24195L; `ctrl_power_pmic` semantics as above |
| `health.cpp` | `fault_out` drives relay 3 instead of D5; same HIGH/LOW semantics, now a dry loop with no external module |
| `pins.h` / `WIRING` / `docs/hardware.md` | the table above |
| Web console | esptool-js replaces `samba.js` for commissioning; the Install/Firmware tab streams signed images over the console; "Shelly"/"Alarm.com" naming rule unchanged |
| `tools/flash.py`, `/flash` | esptool for the first flash, the console update after; the version check stays |
| Bench | GateSim unchanged (it drives 24 V-wetted contacts and reads dry contacts); `tools/bench-wiring/wiring.json` redrawn; all MKR timing numbers in the docs re-measured |
| Threat model | the four physical-access rows move to Mitigated; add `rui3-firmware-trust` (a second firmware in the RF path) and `console-image-update` rows |

Rough size: XL, as H2 says. It is several MINORs: F11 (no behaviour change), the board port with the radio, then the
console update path. Nothing in `link.cpp`, the role state machines or the host-test invariants changes, which is the
point of doing F11 first.

## What to verify before deciding

1. On a StamPLC + U184 pair on the bench: P2P at 915 MHz / SF9 / 500 kHz / CR 4/6 / sync 0x1424 against an MKR, and
   against a second pair; RX→TX turnaround, `AT+CAD` behaviour under a jammer (`tests/e2e` has the runs), and
   whether `+EVT:TXP2P DONE` is reliable enough to replace the TX deadline.
2. Relay drop on `RSTN`: scope the coil when G3 is pulled low with a relay energised, and with SDA held low (bus
   hang) by a jumper.
3. Input thresholds: 12 V and 24 V into the EL3H4 through the 6.2 kΩ series pair; confirm "off" with the wire cut
   and with 24 V present but the contact open.
4. Boot time to first `STATUS` with the display driver in the loop; it must stay under `BOOT_SETTLE_MS` logic's
   assumptions and well inside the 8 s watchdog.
5. Enable flash encryption + Secure Boot v2 in Release mode on one unit and prove: esptool cannot read the flash
   back in plaintext, an unsigned image does not boot, a signed image delivered over the console does.
6. Current draw at 22 dBm TX from the Grove 5 V with two relays energised, on 12 V and on 24 V input.

## Sources

- M5Stack StamPLC (K141): https://docs.m5stack.com/en/products/sku/K141 and https://docs.m5stack.com/en/core/StamPLC
  (specs, pin map, AW9523B/EL3H4/HF3FF/ULN2003A, schematics); launch price $42.90:
  https://www.cnx-software.com/2025/03/22/m5stack-stamplc-esp32-s3-plc-opto-isolated-inputs-relay-ouputs-rs485-can-bus/
- AW9523B datasheet (power-on defaults by AD1/AD0, Table 1; RSTN):
  https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1129/AW9523B.pdf
- M5Stack Unit LoRaWAN-US915 (U184): https://docs.m5stack.com/en/products/sku/U184-US915
- RAK RUI3 AT command manual (P2P: `AT+NWM`, `AT+P2P`, `AT+PRECV`, `AT+PSEND`, `AT+CAD`, `AT+SYNCWORD`, `+EVT:RXP2P`):
  https://docs.rakwireless.com/product-categories/software-apis-and-libraries/rui3/at-command-manual/
- StamPLC IO Module (A176): https://docs.m5stack.com/en/products/sku/A176
- M5Stack 4-Relay Unit (U097): https://docs.m5stack.com/en/unit/4relay; Module LoRa868 v1.1 (M029-V11):
  https://docs.m5stack.com/en/products/sku/M029-V11; Unit C6L: https://meshtastic.org/docs/hardware/devices/m5stack/unit-c6l/
- ESP32-S3 flash encryption and Secure Boot v2 enablement:
  https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32s3/security/flash-encryption.html and
  https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/security/security-features-enablement-workflows.html
- SX1276 ↔ SX126x interoperability and sync-word mapping:
  https://forum.lora-developers.semtech.com/t/sync-word-for-sx1262/183 and
  https://forum.lora-developers.semtech.com/t/sx126x-127x-interoperability-at-500khz/1568/6
- esptool-js: https://github.com/espressif/esptool-js/
- MKR WAN 1310 and Relay Proto Shield prices: https://uk.rs-online.com/web/p/microcontroller-development-tools/2024162,
  https://store.arduino.cc/en-cz/collections/all/products/arduino-mkr-relay-proto-shield
