# Bench wiring

A local page showing every wire on the bench: both boards, the GateSim, the power rig and the PC. It's the
agreed record of how the bench is wired, kept in `wiring.json` and edited by both of us: in the page, or
directly in the JSON.

```sh
python -m http.server 8001 -d tools/bench-wiring   # then open http://localhost:8001 in Chrome or Edge
```

- **Statuses:**
  - `connected`: confirmed by eye, a meter, or the e2e suite.
  - `unverified`: believed to be there, from the docs, but not checked.
  - `planned`: agreed but not wired yet.
  - `open`: deliberately unconnected.
- **Editing:** click **Open wiring.json…** and pick this folder's `wiring.json`. Status, terminal, signal and note
  edits then save straight back to the file (Save or Ctrl+S). That needs Chrome or Edge; other browsers can use
  **Download**. **Reload** re-reads the file after someone else changed it.
- **Diagram:** one line per pair of devices, coloured by its least-settled wire. Click a device to show only its
  wires.
- Add devices or sites in the JSON directly (`devices`, `sites`). A wire is `{a: [device, terminal], b: [device,
  terminal], signal, status, note}`.

Keep it in step with the bench: when a wire moves, change it here in the same session.
