# Installing on site

> **Draft, to finish on site.** The site survey below is ready to use. The checklist after it is a skeleton built
> only from [hardware.md](hardware.md) and the [README](../README.md); walk it at the install and fill in what only
> the real hardware shows (see [Still to fill in](#still-to-fill-in)). Tracked in [TODO.md](../TODO.md) ("Install
> checklist").

## Site survey

The survey pings the other board for a few minutes and judges how much margin the radio link has in each
direction, so you know before the boards are fixed in place whether the link will hold up through rain, foliage
and interference.

### When

- **Before fixing the boards in place**, with the gate board at its final spot and both antennas mounted where
  they will stay (outside the enclosure on the U.FL→SMA pigtail, away from the relay shield and field wiring, as
  [hardware.md](hardware.md#power-and-antenna) says).
- Both boards powered, with their roles, the same key and the same radio settings, and the link up (Status:
  *Peer verified: yes*; README, First-time setup).
- Again after moving an antenna or changing a radio setting. A second survey on another day (after rain, or with
  the trees in leaf) shows how much the margin moves.

Only one board needs a laptop: the survey runs from either. From the house is slightly better: the house knows the
gate's noise floor (the gate sends it in its STATUS), so both directions can be judged on strong links; from the
gate, the house's reception is judged by its SNR alone, which only understates a strong link.

Power the board you survey from with its own supply (or LiPo) and use USB for the console. On a laptop's USB power
alone, a full-power transmit with a relay energized has reset boards (hardware.md, Power and antenna).

### How

**Web console:** Tools → **Site survey**. Choose 1, 2 or 5 minutes and **Start survey**. It sends a ping every
2 s, one at a time (the board reports a pong only for its latest ping); a ping without an answer within 8 s counts
as lost.
The ping card pauses while it runs (auto-ping resumes after). **Stop survey** ends it early and keeps what it has; so
does a disconnect. **Copy report** puts a plain-text report on the clipboard for the install record.

**Command line** (needs Python and pyserial; close the web console first):

```sh
python tools/gatelink.py survey                        # the only board on USB, or the house if both are
python tools/gatelink.py survey gate --seconds 300     # from the gate, for 5 minutes
python tools/gatelink.py survey COM5 --json survey-house.json
```

`--interval` sets the seconds between pings (default 2) and `--seconds` the length (default 120); Ctrl+C stops
early and still reports. `--json FILE` writes the full report: settings, every ping, summary, verdict and advice,
the board's role, firmware and USB serial number, and the time. Keep it with the install record. The command exits 0
for a good or fair link and 1 for a marginal or poor one, or if a board error cut the survey short (it still
reports what it had).

### Reading the result

The table has a row per direction, each what that board received (*at the gate* = the house's pings heard at the
gate): pings received, RSSI min / median, SNR min / median, and the **margin** min / 10th percentile.

The margin is how far each frame was above the SX1276's demodulation limit for the spreading factor (Semtech
datasheet): below the limit a frame is lost.

| SF | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|
| SNR limit (dB) | −5 | −7.5 | −10 | −12.5 | −15 | −17.5 | −20 |

With the SNR below 5 dB the margin is SNR minus the limit. From 5 dB up the SNR reading saturates (near +10 dB on a
strong link), so when the receiving board's noise floor is known the margin is the larger of that and RSSI minus
the noise floor minus the limit.

The verdict rests on the weaker direction's **10th-percentile** margin (90 % of pings had at least this):

| Verdict | When |
|---|---|
| Good | 15 dB or more |
| Fair | 10 to 15 dB |
| Marginal | under 10 dB |
| Poor | more than 5 % of pings unanswered (and at least 2 of them), whatever the margin; above 2 % the advice says frames are being lost |

With fewer than 20 pings answered the result is rough: survey longer.

### What to change

- **Good:** nothing. If the margin is well above 15 dB, `tx_power` could come down on both boards (the advice says
  by how much); that's optional.
- **Fair:** usable. If it's easy, mount the antennas higher with a clearer line of sight, and survey again.
- **Marginal or poor:** in this order, surveying again after each:
  1. Antennas: higher, clear of metal, walls and the relay shield, with as clear a line of sight between the boards
     as you can get. This gains more than any setting.
  2. `tx_power` up on both boards (at most 20 dBm), each board on a solid supply.
  3. `sf` up on **both** boards: each step adds about 2.5 dB of margin and roughly doubles the airtime. Radio
     settings must match, and they can't be written over the link, so change one board, then the other over its
     own USB: the link is down in between.
- **Pings lost with a good margin:** frames are being lost to something else. Check the antenna cables and the
  noise floor (Status → Noise floor on each board; on the house, Tools → Link history also shows the gate's).

Keep the report: the command-retry item in [TODO.md](../TODO.md) waits on real install-site RF numbers.

## Install checklist

A skeleton: each step is from [hardware.md](hardware.md) or the README. Check every wire against the Install tab of
the web console (it works without a board) before powering up.

### Before going

- [ ] Both boards on the same firmware, roles set and saved (house, gate), the same key written to both, and the
  link verified on the bench (README, First-time setup steps 1–5).
- [ ] A copy of the key somewhere safe. The boards can't read it back and the config export leaves it out.
- [ ] A laptop with Chrome or Edge (Web Serial), or Python with pyserial for `tools/gatelink.py`, and a USB cable.

### Gate board

- [ ] Power: a 24 V→5 V buck from the opener's 24 V accessory output (battery-backed) or the AC 24 V supply (dies
  with AC: add a LiPo). MKR VIN is 5 V max.
- [ ] CSW24UL: AUX relay A set to *open limit*, AUX relay B to *closed limit* (LiftMaster manual).
- [ ] Opto board: OUT1–OUT4 → IN1–IN4 (OUT4 spare), output GND → board GND, output VCC → the board's **3.3 V only**. 24 V side:
  the accessory output's 24 V → AUX C; open-limit AUX NO → channel 1; closed-limit AUX **NC** → channel 2;
  channel 3 across the 24 V supply on AC (the one feeding the AES controller).
- [ ] K1 NO/COM → OPEN + COM, K2 NO/COM → CLOSE + COM.
- [ ] `in1_invert`–`in4_invert` off.
- [ ] Status on the gate board: *closed* at the close limit, *open* at the open limit, *between* while it moves;
  the I/O card shows IN1 (open limit) and IN2 (closed limit) lit at their limits.
- [ ] AC power sense (`power_sense` on): unplug the AC supply: the I/O card shows IN3 (AC power) off, and commands are refused
  (the house shows *refused: no AC power*); plug it back. If IN3 isn't wired, turn `power_sense` off.
- [ ] Tools → Relay test, with someone watching the gate: K1 opens it, K2 closes it.

### House board

- [ ] Power: a 12 V→5 V buck from the Shelly's 12 V supply (`ctrl_power_pmic` needs the board fed from it), and a
  LiPo so the board stays up through a 12 V cut.
- [ ] Shelly Wave 1 on 12 V DC. Relay output I → 3.3 V, O → HOUSE IN1. SW input set to *toggle switch, contact
  closed = ON / open = OFF* ("SW1 switch type" in the Shelly Wave 1 manual), not detached mode.
- [ ] K1 NO/COM → the Shelly's SW input.
- [ ] K2 NO/COM → the 2GIG contact sensor's terminals; the sensor named "Gate" in Alarm.com (`sensor_invert` if
  it reads the wrong way).
- [ ] IN2: an opto channel rated for 12 V across the Shelly's 12 V supply, output side from the board's 3.3 V.
- [ ] Status on the house board: *Controller input* follows the Alarm.com switch, *Controller power* on.
- [ ] Controller power sense (`ctrl_power_sense` on): with the LiPo in, cut the Shelly's 12 V. The house log shows *controller power lost*
  (`ctrl_power 0`) and no command is sent; *Supply* shows *lost · on battery*. Restore it. If IN2 isn't wired, turn
  `ctrl_power_sense` off.
- [ ] If the Alarm.com switch flips whenever the gate moves (house log: `sync`, then repeated `resync`), the SW
  input is toggling on every edge: fix its mode.

### Radio

- [ ] Antennas attached before the radios transmit, outside the enclosures, away from the relay shields and field
  wiring.
- [ ] Site survey (above): good or fair before fixing the boards in place. Keep the report.
- [ ] `tx_power` as the survey settled it, saved on both boards. Bench boards are saved at about 5 dBm (USB
  power). Watch Status → *Last reset* and the radio fault count for a few days after (TODO.md, "Full power at the
  install").
- [ ] `uart_console` off and pins 13/14 unused.

### End to end

- [ ] With someone watching the gate (README, First-time setup step 7): switch on in Alarm.com, and the gate
  opens, the house Status follows (*between*, then *open*) and the contact sensor reports open. Switch off: it
  closes, and the sensor reports closed.
- [ ] Move the gate another way (keypad or remote): the switch follows, and the gate isn't commanded back.

### Record

- [ ] Config → **Export JSON** on each board (or `python tools/gatelink.py snapshot`).
- [ ] The key: check the stored copy is the one written to these boards (the link verified with it), and that it is
  kept somewhere safe. If it's ever lost, `key.set` a fresh one on both boards.
- [ ] The survey report (copied text or `--json` file) with the exports.

## Still to fill in

- Photos of each board's wiring and the antenna mounting.
- The Shelly and CSW24UL menu steps as done on site, and any settings changed from the defaults.
- The readings at each check (limits, power senses, survey margins), as a baseline for later troubleshooting.
- Anything the steps above got wrong or left out.
