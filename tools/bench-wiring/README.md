# Bench wiring

A local wiring diagram of the whole bench: both boards, the GateSim, the power rig and the PC. It's the agreed
record of how the bench is wired. The data lives in `wiring.json`, which both of us edit: in the page, or directly
in the file.

```sh
python tools/bench-wiring/serve.py   # then open http://localhost:8001; Save writes wiring.json in place
```

Always serve the page with `serve.py`, never `python -m http.server` or another static server: only `serve.py`
accepts the save, so under any other server Save asks for the file. The page shows a warning banner when it isn't
served by `serve.py`. If port 8001 is taken by another server, stop that server rather than picking another port.

## Reading the diagram
- **Rows:** one container per site (Gate, with the GateSim and its 4-channel relay module; House). Names inside a container don't
  repeat the site: "LiPo" in the Gate container is the gate LiPo.
- **Boxes:** each device is a box with power terminals on top, inputs on the left, outputs on the right, and
  comms (USB, UART) at the bottom. Relay contacts are always on the right, each COM next to its NO/NC. Opto
  boards have a `+`/`−` input pair per channel and their voltages (in → out) as a subtitle. Side terminals line
  up across a row, so a wire between two aligned terminals runs straight.
- **Buses:** common connections (+24 V, +12 V, 0 V/GND, +5 V, 3.3 V, LiPo) are horizontal bus lines above the
  row. A dot marks each terminal tied to a bus.
- **PC strip:** each site has a strip along its bottom, and USB cables drop straight down to it.
- **Lanes:** signal wires run box to box. Wires that would cross a box detour through lanes under the row.
- **Numbered diamonds** mark where a relay module channel breaks a wire: diamond *n* on the wire, and on its CH*n* COM
  and NC (`cut: n` on those wires). A dot beside the diamond marks the side that goes to the relay's COM
  (`com: "from"` or `"to"` on the broken wire).
- **Off-diagram ends** are labelled (in black) at the end of a short stub, also for a wire to the other site's
  row; × marks a terminal that's deliberately unused.

## Wire colour and status
**Colour** is the wire's insulation colour (`color`: red, black, white, yellow, orange, green, blue, brown,
purple, grey, pink). Each bus has a colour too (+24 V and +12 V red, +5 V orange, 3.3 V yellow, ground black,
LiPo pink), shared by the wires dropping onto it. The signal colours are picked per function and kept along the
whole path: open limit green, closed limit blue, AC / controller power sense purple, OPEN pulse white, CLOSE pulse
brown, UART per the FTDI cable (adapter TX orange, RX yellow), USB grey. Change any of them to match the real wire.
A USB cable that carries 5 V has a red core (`usb: "powered"`); a power-blocked one is tagged "data only"
(`usb: "data"`).

**Status** is the dash pattern:
- **Solid:** `connected`, confirmed by eye, a meter or the e2e suite.
- **Long dashes:** `unverified`, believed to be there from the docs but not checked.
- **Short dashes:** `planned`, agreed but not wired yet.
- **Sparse dots:** `open`, deliberately unconnected.

## Editing
- **In the page:** click a wire to open its card and set its status, colour or note; click off it to close the card.
  Save or Ctrl+S writes the served `wiring.json` (served by `serve.py`, see above). As a fallback, without
  `serve.py`, the first save asks for the file (Chrome or Edge; the dialog remembers the folder), or use
  **Open wiring.json…** up front. **Reload** re-reads the file after an edit made elsewhere.
- **In the JSON:** structural changes go here.
  - A device has `top`, `left`, `right` and `bottom` terminal lists; `""` in a list is an empty row (a spacer to line
    terminals up, as with OUT*n* beside *n*+). `sub` is an optional subtitle under the name. `bench: true` marks a
    device that's only part of the bench test rig, not the install (teal border). `gap` adds that many pixels
    before the box, e.g. to fit stub labels between two boxes.
  - `bands` sets each row's device order.
  - `comms` places a device under another one. When its top terminals wire to the bottom of the box above in the
    same order (FTDI TX/RX/GND to 13 RX/14 TX/GND), it is sized and shifted so those wires drop straight down.
  - A wire is `{from: "device:terminal", to: "device:terminal" | "pc", bus: id, label: text, via: text, status,
    color, signal, note}` (`via` is drawn near the `to` end). Give it one of `to`, `bus` or `label`; with none of them, the terminal is shown unconnected.

Keep it in step with the bench: when a wire moves or gets confirmed, change it here in the same session.
