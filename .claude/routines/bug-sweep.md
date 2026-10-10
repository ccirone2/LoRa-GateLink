---
name: bug-sweep
schedule: 0 3 * * *
description: Look for bugs and simplifications in one area a night; fix one, proven by a test, in a small pull request
---

# Nightly bug sweep

1. **Pick tonight's area** from the day of the year (`date +%j`, modulo 12):
   0. `link.cpp` (framing, replay window, sessions, retries, listen-before-talk)
   1. `role_gate.cpp` (relays, limits, AC power, STATUS, remote config)
   2. `role_house.cpp` (K1 sync, controller power, K2, link loss)
   3. `console.cpp` and `console_io.cpp`
   4. `config.cpp`, `histlog.cpp` and `extflash.cpp` (the SPI flash)
   5. `radio.cpp`, `supply.cpp`, `history.cpp`, `health.cpp`, `app.cpp`
   6. `web/js/serial.js`, `samba.js`, `firmware.js`
   7. `web/js/status.js`, `config.js`, `history.js`, `settings.js`
   8. `tools/gatelink.py`, `tools/gatelink_client/`, `tools/release_evidence.py`
   9. `tests/e2e/gatelink/` (the suite's own correctness: checks that can't fail, races in waits)
   10. `tests/native/` and `tests/web/` (fakes that disagree with the real thing)
   11. `.github/workflows/`, `tools/agent/` (hooks), `tools/check_contract.py`
2. **Read it closely** against the rules in `CLAUDE.md` (behavioural invariants, wrap-safe timing, non-blocking
   loop, the console contract) and look for: invariant breaks, wrap or overflow, unchecked lengths, state left
   behind on an error path, a retry or timer that can stall, dead code, and code that a simpler form would make
   clearer without changing behaviour. Check `TODO.md` first: a known item is not a new finding.
3. **Prove before fixing.** A bug counts only with a test that fails on `main`: the host tests
   (`make -C tests/native`, including the system tests' invariant monitors), the web tests (`npm test`) or the
   tools tests (`python -m pytest tests/tools -q`). Write the test, see it fail, fix, see it pass, run the area's
   whole suite. A simplification needs the existing tests to cover what it touches (add one if not) and must not
   change behaviour.
4. **One pull request** for the most valuable proven item, titled `[routine] Fix: ...` or `[routine] Simplify:
   ...`, on `claude/routine-bug-sweep-<yyyymmdd>`. Firmware: bump `FW_VERSION` (PATCH), draft, "needs bench".
   Other real findings you couldn't prove on the host, or didn't fix tonight, go into `TODO.md` in the same pull
   request (or, with no pull request, in the summary) with the evidence: file:line and the scenario.
