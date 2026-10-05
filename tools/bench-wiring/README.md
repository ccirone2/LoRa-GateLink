# Bench wiring

A local wiring diagram of the whole bench: both boards, the GateSim, the power rig and the PC. It's the agreed
record of how the bench is wired. The data lives in `wiring.json`, which both of us edit: in the page, or directly
in the file.

```sh
python -m http.server 8001 -d tools/bench-wiring   # then open http://localhost:8001 in Chrome or Edge
```

## Reading the diagram
- **Rows:** one per site (gate side with the GateSim, house side).
- **Boxes:** each device is a box with power terminals on top, inputs and outputs on its left and right, and
  comms (USB, UART) at the bottom.
- **Buses:** common connections (+24 V, +12 V, 0 V/GND, +5 V, 3.3 V, LiPo) are horizontal bus lines above the
  row. A dot marks each terminal tied to a bus.
- **PC strip:** each site has a strip along its bottom, and USB cables drop straight down to it.
- **Lanes:** signal wires run box to box. Wires that would cross a box detour through lanes under the row.
- **Off-diagram ends** are labelled at the end of a short stub; × marks a terminal that's deliberately unused.

## Wire status (line style)
- **Solid:** `connected`, confirmed by eye, a meter or the e2e suite.
- **Orange dashes:** `unverified`, believed to be there from the docs but not checked.
- **Blue dots:** `planned`, agreed but not wired yet.
- **Grey dots:** `open`, deliberately unconnected.

## Editing
- **In the page:** click a wire to open its card and set its status or note. To save straight back to the file,
  click **Open wiring.json…** once and pick this folder's `wiring.json`, then use Save or Ctrl+S (Chrome or
  Edge). **Reload** re-reads the file after an edit made elsewhere.
- **In the JSON:** structural changes go here.
  - A device has `top`, `left`, `right` and `bottom` terminal lists.
  - `bands` sets each row's device order.
  - `comms` places a device under another one.
  - A wire is `{from: "device:terminal", to: "device:terminal" | "pc", bus: id, label: text, via: text, status,
    signal, note}`. Give it one of `to`, `bus` or `label`; with none of them, the terminal is shown unconnected.

Keep it in step with the bench: when a wire moves or gets confirmed, change it here in the same session.
