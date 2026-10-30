# Proposal: a RAK WisBlock hardware architecture for GateLink

Status: proposal (2026-10-30), not decided. The alternative to the M5Stack design in
[m5stack-hardware.md](m5stack-hardware.md); both answer ROADMAP [H2](../../ROADMAP.md). Read that one first: the
key-protection, web-console-flashing and firmware-port sections apply here unchanged and are only referenced.
Prices are RAK store and reseller figures from October 2026 and will drift.

## Summary

Build each end from RAK's modular WisBlock system around the **RAK3312** core (ESP32-S3 + SX1262 on SPI):

| | Today (MKR) | Proposed (WisBlock) |
|---|---|---|
| Core | SAMD21 + Murata SX1276 module, key in plain-text SPI flash | RAK3312: ESP32-S3 (16 MB flash, 8 MB PSRAM), SX1262 on SPI, native USB; flash encryption + Secure Boot v2 |
| Base | MKR Relay Proto Shield | RAK19011 base (power slot, 2 IO slots, 6 sensor slots) |
| Power | 5 V VIN max, a buck at each end, LiPo on the MKR | RAK19016 power slot: 5–24 V screw terminals, LiPo charger (ride-through kept) |
| Relays | 2 on the shield + D5 needing a relay module | RAK13001 ×2: a 30 V / 2 A relay **and** an isolated 12–24 V input each |
| Inputs | 4 × 3.3 V pins through an external opto board | 2 isolated on the RAK13001s; IN3/IN4 at the gate through a 2-channel opto on the base's IO header, as today |
| Enclosure | bare boards in a box | RAK Unify outdoor enclosure (IP65/67) with the DIN-rail kit |
| Cost per location | ~$95–115 | ~$75–95 (several prices unconfirmed, below) |

The firmware stays one sketch with the role in config; `link.cpp`, the frame format and the state machines don't
change. Unlike the M5Stack route, **`radio.cpp` keeps its shape**: the SX1262 is on SPI under our own driver, so the
listen-before-talk, async-TX-with-deadline, CRC and RSSI/SNR behaviour is ported, not replaced by an AT modem.
ROADMAP F11 (chip-specific code behind `board.h`) is the prerequisite either way.

## Why WisBlock, and where it fits badly

RAK is the only vendor in this space with an SX1262 core that is also an ESP32-S3 (the RAK3312), a 5–24 V power
module with a battery, relay-plus-isolated-input modules and an outdoor DIN-mountable enclosure, all screwed to one
base with no soldering, and the SX126x family is what `radio.cpp` would move to anyway (lower RX current, +22 dBm).
The cores are cheap ($15) and the modules are $10–20 each.

Where it fits badly, honestly:

- **The I/O catalogue is thin for a controller.** The only isolated-input module is the RAK13001 (one relay, one
  opto input); the RAK13007 is one bigger relay; the RAK13003 is a 3.3 V MCP23017 expander. There is no multi-channel
  isolated-input module and no multi-relay module. A gate end needs 2 relays, 3–4 inputs and a fault output.
- **Both IO slots carry identical pins.** The RAK13001 and RAK13007 drive their relay from IO4 (WisConnector pin 32)
  by default, and the RAK13001 reads its input on pin 10 by default. Two of them on one base need the second module
  reworked to its documented alternate pins (`DO_Option` 36, `DI_Option` 35, the I2C2 lines, by moving 0402
  resistors). RAK's public datasheet names the pins but not the resistors: ask RAK, and record the rework in
  `docs/hardware.md`. It is one soldering step per site, which the M5Stack design avoids.
- **No 5 V rail.** The RAK19016 gives 3.3 V (and the LiPo); a third-party listing says 0.6 A. Budget: ESP32-S3
  ~80 mA, LoRa TX up to 140 mA at 22 dBm, two RAK13001 coils at 70 mA each when both relays are held (house K1 and
  K2 can be): ~370 mA, inside the limit but to be measured, with the LiPo buffering TX peaks. Keep `tx_power`
  moderate until measured.
- **24 V absolute maximum on the RAK19016.** The CSW24UL's battery-backed accessory output can float above 24 V
  on charge. Measure it; if it exceeds 24 V, feed the gate end from a small 24→12 V regulator (~$5) or from the
  AC-side 24 V supply with the LiPo for ride-through (then `power_sense` on IN3 still reports AC loss).

The other RAK routes were checked and don't fit: WisDuo modules (RAK3172, RAK4630) need a custom carrier PCB; the
RAK7431 and the WisNode line are RS485/sensor bridges with no relays or inputs; RUI3 (the RAK4631-R and RAK3372)
is RAK's own firmware with its own Arduino API, so the stack would be theirs, not ours.

## Parts per location

| Part | Role | Price (Oct 2026) |
|---|---|---|
| RAK3312 (H, US915) | core: ESP32-S3, SX1262, USB | $15.00 (RAK store) |
| RAK19011 | base: power slot, 2 IO slots, 6 sensor slots, USB-C (verify: the RAK19001 it extends has one) | ~$15–18 (RAK19001 is $13.50; RAK19011 unconfirmed) |
| RAK19016 | 5–24 V power slot module with LiPo charger | ~$10 (unconfirmed) |
| RAK13001 ×2 | relay + isolated input, one reworked to the alternate pins | ~$10–12 each (unconfirmed) |
| LiPo 3.7 V ≥500 mAh | ride-through at the house; TX buffering at both | ~$8 |
| RAKBox-UO100x75x38 (Unify) + DIN kit | outdoor enclosure, DIN rail | ~$20–30 (unconfirmed) |
| 2-channel PNP opto board (gate only) | IN3 AC sense, IN4 closed sensor | ~$5 |
| Antenna + SMA bulkhead | | ~$8 |
| **Total** | | **~$75–95 house, ~$80–100 gate** |

Cores, bases and modules are also on DigiKey and Mouser, which matters for "reproducible by others".

## Wiring

| | House | Gate |
|---|---|---|
| RAK13001 #1 relay (IO4) | K1 → Shelly SW input (energized = not closed) | K1 → CSW24UL OPEN + COM (pulsed only) |
| RAK13001 #1 input (12–24 V, wet) | IN1: Shelly relay output O, I ← Shelly 12 V + | IN1: AUX1 open-limit NO, wetted from the accessory 24 V |
| RAK13001 #2 relay (reworked, pin 36) | K2 → 2GIG contact sensor (energized = closed) | K2 → CSW24UL CLOSE + COM (pulsed only) |
| RAK13001 #2 input (reworked, pin 35) | IN2: Shelly 12 V + (`ctrl_power_sense`) | IN2: AUX2 "not at close limit" **NC**, wetted from the accessory 24 V |
| Base IO header IO5, IO6 (3.3 V, pull-down) | spare | IN3 AC sense, IN4 closed sensor, through the 2-ch PNP opto (3.3 V out, as today) |
| Base IO header IO1 | `fault_out` (3.3 V logic) → relay module → second 2GIG sensor, as today | same |
| RAK19016 VCC-IN | Shelly 12 V | opener accessory 24 V (see the maximum above) |
| USB-C | web console | web console |

The RAK13001 input is a wet-contact input (12–24 V across it), which is exactly how the limits and the Shelly are
wired today: a lit opto reads active, a dead opener, cut wire or dead channel reads off (fail toward not-closed). The
relay contacts are 30 VDC / 2 A: fine for the opener's dry OPEN/CLOSE inputs, the Shelly SW input and a 2GIG
sensor loop. The CSW24UL AUX settings and the NC closed-limit reasoning in `docs/hardware.md` carry over.

Relays fail de-energized: the ESP32-S3's GPIOs are high-impedance through reset and the RAK13001's driver is off
with its input floating; the coil runs from `3V3_S`, which the core switches with IO2 and the board layer turns on
only after the relay pins are driven low (the "relays de-energised first" step of `setup()`).

## Firmware changes, by layer

Same plan as the M5Stack proposal, with these differences:

| Layer | WisBlock specifics |
|---|---|
| `radio.cpp` | SX1262 port of the existing driver (RadioLib or a thin SPI layer): `BUSY` on GPIO48, `DIO1` on GPIO47, `NRESET` on GPIO8, the RF switch on GPIO4; LBT from CAD, TX-done from DIO1, RSSI/SNR/CRC kept; the SX1262 has no FEI register in LoRa mode (`fei` leaves the history, as on the M5Stack route) |
| `board.h` / `io.cpp` | relays and the two isolated inputs are plain GPIOs again (no I2C expander, no reset-on-hang mitigation needed); IN3/IN4 and `fault_out` on header pins with internal pull-downs, as on the MKR |
| `supply.cpp` | RAK19016 has no power-good readback: sense VIN with a divider on AIN0 (GPIO1) and keep `ctrl_power_pmic` as "VIN below threshold"; the LiPo keeps the house up through a controller cut, so today's ~0.2 s early warning survives |
| `extflash.cpp` / `config.cpp` | encrypted-flash partition, as in the M5Stack proposal; saves no longer silence the radio |
| Web console | esptool-js for commissioning, signed images over the console afterwards (M5Stack proposal, "Key protection") |
| Threat model | the four physical-access rows to Mitigated; no second firmware in the RF path (unlike RUI3) |

## WisBlock cores considered

| Core | Radio | Key protection | Firmware route | Verdict |
|---|---|---|---|---|
| **RAK3312** | SX1262, SPI | ESP32-S3 flash encryption + Secure Boot v2 | Arduino (Espressif BSP), our driver | **chosen** |
| RAK4631 | SX1262, SPI | nRF52840 APPROTECT only: bypassed by voltage glitching on revisions 1–2 (CVE-2020-27211), hardened on revision 3; no flash encryption | Arduino (Adafruit-style BSP), SX126x-Arduino | second choice if the ESP32-S3 proves troublesome; weaker H2 answer |
| RAK3372 | STM32WLE5 (SX126x core) | RDP level 2 (permanent) | RUI3 only | RAK's firmware stack, not ours |
| RAK11310 | SX1262, SPI | none (RP2040 has no readout protection) | Arduino-Pico | no H2 benefit |
| RAK11162 | STM32WLE + ESP8684 | RDP + ESP | RUI3 | as RAK3372 |

## Against the M5Stack design

| | M5Stack (StamPLC + U184) | RAK WisBlock (RAK3312 stack) |
|---|---|---|
| Soldering / rework | none | one RAK13001 reworked per site |
| Inputs | 8 isolated, 5–36 V | 2 isolated 12–24 V + 3.3 V header pins through an opto |
| Relays | 4 × 5 A | 2 × 2 A (+ a relay module for the fault loop) |
| Power | 6–36 V, no battery | 5–24 V (absolute), LiPo ride-through |
| Radio control | AT modem (RUI3), loses FEI and CRC counters | our SPI driver on the SX1262, keeps everything but FEI |
| Radio failure mode | a second firmware; recover with `ATZ`/reset | as today (`radioRecover`) |
| Display / buttons for the installer | yes | no (an OLED module exists, RAK1921) |
| Enclosure | DIN case, screw terminals | IP65/67 outdoor box with DIN kit, modules on standoffs |
| Price | ~$70–90 | ~$75–100 |
| Availability | M5Stack, DigiKey | RAK store, DigiKey, Mouser |

In one line: M5Stack gives the installer the better box and more I/O with no soldering; WisBlock gives the developer
the radio under our own driver, the LiPo and the cheaper core, at the cost of a resistor rework and thinner I/O.

## What to verify before deciding

1. RAK: the resistor positions for the RAK13001's `DO_Option` (pin 36) and `DI_Option` (pin 35), and whether a
   RAK19011 has a USB-C receptacle.
2. Measure the CSW24UL accessory output on charge against the RAK19016's 24 V maximum.
3. Current from the RAK19016's 3.3 V with both relays held and the SX1262 at 22 dBm; `radio_faults` and resets over
   the `tests/e2e` ping run (today's "TX with a relay energised" bench rule, re-measured).
4. SX1262 ↔ SX1276 mixed pair at SF9 / 500 kHz (the migration case, see the M5Stack proposal).
5. Flash encryption + Secure Boot v2 in Release mode on one RAK3312; a signed image delivered over the console.
6. The Unify enclosure with base, two RAK13001s, LiPo and antenna bulkhead fitted, on the DIN kit.

## Sources

- WisBlock catalogue: https://docs.rakwireless.com/product-categories/wisblock/
- RAK3312 datasheet (pins, SX1262 wiring, power): https://docs.rakwireless.com/product-categories/wisblock/rak3312/datasheet/;
  price: https://store.rakwireless.com/products/wisblock-core-module-rak3312-lora-wifi-ble
- RAK19011 datasheet (identical IO slots): https://docs.rakwireless.com/product-categories/wisblock/rak19011/datasheet/;
  RAK19001 price: https://store.rakwireless.com/products/wisblock-base
- RAK19016 datasheet (5–24 V, LiPo, 3.3 V): https://docs.rakwireless.com/product-categories/wisblock/rak19016/datasheet/
- RAK13001 datasheet (HF46F relay, 12–24 V opto input, `DO_Option`/`DI_Option`): https://docs.rakwireless.com/product-categories/wisblock/rak13001/datasheet/
- RAK13007 datasheet: https://docs.rakwireless.com/product-categories/wisblock/rak13007/datasheet/
- RAK13003 datasheet: https://docs.rakwireless.com/product-categories/wisblock/rak13003/datasheet/
- RAK11310 datasheet: https://docs.rakwireless.com/product-categories/wisblock/rak11310/datasheet/
- Unify enclosure: https://docs.rakwireless.com/product-categories/wisblock/rakbox-uo100x75x38/datasheet/
- WisBlock core modules and RUI3: https://store.rakwireless.com/products/wisblock-core-modules,
  https://docs.rakwireless.com/product-categories/software-apis-and-libraries/rui3/arduino-api
- nRF52840 APPROTECT: https://nvd.nist.gov/vuln/detail/CVE-2020-27211, https://infocenter.nordicsemi.com/pdf/in_141_v1.1.pdf
