# Host tests: the firmware on the PC

The firmware built with g++ and run on the PC, two ways:

- **`tests`**: `link.cpp` and `config.cpp` on their own (`test_link.cpp`, `test_config.cpp`): replays across sessions
  and restarts, HELLO side effects, retry spacing, timer wraps, power cuts mid-save, records from other firmware.
- **`systests`**: both boards' **whole firmware** in a simulated site (`world.h`, `test_system*.cpp`): the opener, the
  controller (Shelly) and its power, the contact sensor and the radio channel, on one clock, with monitors that check
  the behavioural invariants (CLAUDE.md) every simulated millisecond. Every invariant has tests here that fail if it
  breaks; `tools/mutate.py` checks that by breaking each one in a copy of the firmware.

Nothing here talks to hardware; the bench suite in `tests/e2e` remains the end-to-end check on the real boards.

```sh
make -C tests/native                    # builds and runs both; exit status 1 on a failure
make -C tests/native run-systests       # just the simulated site (JOBS parallel shards, default nproc)
make -C tests/native build/tests && tests/native/build/tests retries        # one test (name substring)
make -C tests/native build/systests build/node.so && tests/native/build/systests travel
```

`make` also replays the fuzz corpus (`fuzz/corpus`, and the known-bug reproducers in `fuzz/crashes`, which must
still crash) with g++. The libFuzzer targets for the frame, console and config parsers need clang; see
[fuzz/README.md](fuzz/README.md).

The firmware's libraries come from `LIBS` (default `~/Arduino/libraries`, where `arduino-cli` installs them on Linux,
as in CI): Crypto and ArduinoJson (`CRYPTO=`, `JSON=` override each). On Windows they're in
`~/Documents/Arduino/libraries`, and with no g++ installed the tests run in Docker, e.g. with the `gatelink-dev` image
(`docker build -t gatelink-dev tests/native/docker`):

```sh
MSYS_NO_PATHCONV=1 docker run --rm --security-opt seccomp=unconfined -v "$(pwd -W):/src" \
  -v "$(cd ~/Documents/Arduino/libraries && pwd -W):/libs:ro" -v gatelink-build:/build \
  -w /src/tests/native gatelink-dev make LIBS=/libs BUILD=/build -j4
```

They build with AddressSanitizer and UBSan, and with `-Werror` for our files (third-party headers are system
headers). `make` runs them with address randomization off (`setarch -R`) when it can: under kernels with
`vm.mmap_rnd_bits` = 32, such as Docker Desktop's, GCC's ASan sometimes hangs at startup printing
`AddressSanitizer:DEADLYSIGNAL`. In Docker, `setarch -R` needs `--security-opt seccomp=unconfined`.

## The simulated site (`systests`)

- **Each board is a copy of `build/node.so`**: the firmware (`app.cpp`, `link.cpp`, both roles, `io.cpp`, `log.cpp`,
  `history.cpp`, `console.cpp`, `config.cpp`) plus `node/api.cpp`, its entry points. `world.cpp` loads one copy per
  board with `dlopen`, so each has its own globals; `-Bsymbolic` keeps each copy's references inside it, and
  `-fno-gnu-unique` lets `dlclose` really unload it, so a reset loads it again with fresh RAM. Left out: the drivers
  (`radio.cpp`, `extflash.cpp`, `supply.cpp`, `console_io.cpp`) and `GateLink.ino`. Their functions, and the Arduino
  core's, are answered by `world.cpp` for the board making the call.
- **Time.** One world clock; each board runs one loop pass per simulated millisecond. A board that blocks
  (`delay()`, a flash erase, `LoRa.begin()`, `radioRandom32()`'s 20 ms of sampling) moves its own clock on and isn't
  run again until the world catches up, as a blocked loop would be: relays don't update meanwhile, which is how a flash
  save during a pulse would stretch it. A loop that blocks 8 s without a watchdog reset is a monitor breach and
  resets the board.
- **The site:** `Opener` (the GateSim's model: OPEN/CLOSE inputs debounced, travel time, limits, AC and battery,
  a jammed gate, forced inputs, other controllers pressing its inputs, a siren holding OPEN); `Shelly` (relay = the
  Alarm.com switch = house IN1, SW input = house K1, follow/toggle/detached modes, and its 12 V rail: relay and IN2 opto
  dropping at their bench delays, booting when power returns); the house board's VIN on that rail and the charger's
  power good lagging it; the gate board's feed; LiPos; the radio channel (airtime, half duplex, preamble detection for
  listen-before-talk, collisions, `drop`/`corrupt` hooks, frames of our own with `airSend`); each board's SPI flash,
  which survives resets.
- **Monitors** (`World::monitor`, `World::onLog`) check the invariants all the time: gate relays pulsed only, never
  both, 100 ms interlock, no pulse without AC; every command traced to a user action and every gate pulse to a command or
  a relay test, at most once per command; gate state only from its limits; the contact sensor closed only while the house
  knows the gate closed with the link up, and never lagging the real gate past the link timeout; `no_power`/`fault`
  shown as not-closed; no loop near the watchdog. A breach fails the test (and prints the trace).
- **Writing a test:** `World w; w.commission();` brings both boards up with roles and the shared key and waits for the
  link, the first STATUS and the house's arming. Then act (`w.user(on)`, `w.extPress`, `w.setRail12`, `w.opener.ac`,
  `b.reset(...)`, `w.drop = ...`, `b.request("relay.test", "\"k\":1")`) and check pins (`b.coil(k)`, `w.sensorClosed()`),
  the opener, log events (`b.logs`, `b.count`, `b.last`, `b.waitLog`), console replies and `b.status()` (it's slow: don't
  call it every millisecond in `runUntil`). Name the invariants a test covers in a comment above it, e.g.
  `// [interlock] [pulse-only]`. `w.trace.dump()` prints what happened; it is printed anyway when a test fails.

## Link and config tests (`tests`)

- **`stubs/`** stands in for the Arduino core and FlashStorage. `millis()` is the test's clock (`simNow`), and
  `random()` is seeded, so every run is the same.
- **`sim.cpp`** holds the fakes:
  - The radio (`radio.h`): one SX127x per node on a shared channel, as above. `Sim::drop`, `Sim::corrupt`, `Sim::inject`.
  - The SPI NOR flash (`extflash.h`, sectors 0–3). Programming only clears bits. `cutNextProgram` simulates a
    power cut mid-write; `garbleReads` simulates a garbled bus.
  - `logEvent`, which records events per node.
- **Two nodes, one `link.cpp`.** `link.cpp` keeps its state in file-scope statics, so `link_house.cpp` and
  `link_gate.cpp` compile it into two namespaces (`link_node.inc`). Each `Node` calls its own copy through a
  `LinkApi`. `Node::Ctx` swaps the node's `Config` into the global `cfg` (and `activeRole`) for each call, and
  `link_types.h` includes `link.h` with its function names renamed (argument-dependent lookup would otherwise make the
  namespaced calls ambiguous).
- **`config.cpp`** is compiled once and tested on its own; no node is current.
- Link: build a `Sim`, call `s.handshake()`, then drive the two `Node`s (`sendReliable`, `send`, `ack`, `ackLater`,
  `begin` for a restart, `onRx` to answer as a role would) and check `stats()`, `rx`, `acks`, `logs` and
  `s.sent(node, type)`. Config: call `fresh()` first (blank flash, defaults), then change `cfg`, save, load and inspect
  `flash.mem`.

## Framework (`testing.h`)

A test is `TEST(name) { ... }` with `CHECK`, `CHECK_EQ` and `CHECK_IN`. `XFAIL_TEST(name, reason)` marks a known bug
(an open TODO item): the test must fail, and the run fails once it passes, so whoever fixes the bug flips it to
`TEST`. Both programs take a name substring to run some of the tests.

## Invariant tags

Each system test names the behavioural invariants (CLAUDE.md) it covers in a comment, `// [tag] ...`, and each
mutation in `mutations.json` names the one it breaks. The tags:

| Tag | Invariant | Tests |
|---|---|---|
| `pulse-only` | Gate relays are only ever pulsed (`pulse_ms`, or a relay test's ms), never held | `gate_relays` |
| `interlock` | A pulse never starts within `INTERLOCK_MS` of the other relay releasing, cut short or ended by itself | `gate_relays` |
| `relay-test-target` | A relay test sets a target only if the gate isn't already at that limit (and the opener has power) | `gate_relays` |
| `no-stall-in-pulse` | Nothing that stops the loop (a flash save, a radio restart) runs while a pulse does | `gate_relays` |
| `watchdog` | The loop stays well inside the 8 s watchdog; one console request per port per pass | `gate_relays`, `robustness` |
| `state-from-limits` | Gate state comes only from the opener's limit inputs, never from our last command | `gate_state` |
| `cause` | `lora` only while our pulse's target is being reached; `timeout` at the opposite limit; else `external` | `gate_state` |
| `boot-settle` | The gate's first STATUS waits for steady inputs (`BOOT_SETTLE_MS`, at most 10 s); a boot has no cause | `gate_state` |
| `between-hold` | A move into `between` is held `BETWEEN_HOLD_MS` (`BOOT_SETTLE_MS` out of `no_power`) | `gate_state` |
| `ac-power` | Without AC (IN3) commands are refused `RES_NO_POWER`; a limit is trusted; none reads `no_power` | `gate_state` |
| `spare-before-state` | `updateSpareInputs` runs before `readState()` in `gateLoop` | `gate_state` |
| `k1-mirror` | House K1 makes the controller follow the real gate | `house_sync` |
| `travel-hold` | While the gate is `between`, K1 holds the limit it left until the other or the travel timeout | `house_sync` |
| `sync-window` | Every K1 change opens a sync window; IN1 edges inside it never become commands | `house_sync`, `robustness` |
| `resync` | A lingering mismatch is fixed by cycling K1; at once after a `timeout` result | `house_sync` |
| `armed-after-status` | No command from the controller's level at boot: the first STATUS plus a sync window first | `house_sync` |
| `check-soon` | After boot or controller power return, a controller still out of step is resynced at once | `house_sync` |
| `ctrl-power` | While IN2 or the charger's power good is off, IN1 edges never command and resync pauses; CLOSE waits `ctrl_confirm_ms` | `house_power` |
| `settle-window` | Power return and boot open a `ctrl_settle_ms` window a matching edge can't end early; windows never shorten | `house_power`, `robustness` |
| `unknown-shows-open` | Gate `no_power` or `fault`: the house shows not-closed and commands nothing | `house_power` |
| `sensor-closed-only-known` | House K2 reads closed only while the gate is known closed; it fails open on link loss | `house_power`, `robustness` |
| `wrap-safe` | Timing survives the `millis()` wraps (2^31 for signed comparisons, 2^32) | `robustness` |
| `pull-down` | Inputs are pulled down, active high: a dead opto or cut wire reads inactive | `robustness` |
| `restarts` | Boards, controller and opener restarting in any order recover with no command and no false closed | `robustness` |
| `chaos` | Seeded random sequences of everything at once, every monitor on | `robustness` |
| `busy-channel` | Listen-before-talk: frames get through a busy neighbour's gaps, and never start into a frame heard | `link` |

## Mutation testing (`mutations.json`, `tools/mutate.py`)

A test that can't fail proves nothing. `mutations.json` lists 112 ways to break the invariants above, each an exact
text edit to the firmware (`find` must occur once) with the invariant it breaks, why, and the tests that killed it
(`killed_by`). `tools/mutate.py` applies each to a scratch copy of `firmware/GateLink`, rebuilds the host tests
against it (`make FW=`) and runs its `killed_by` tests, then, if they all pass, the whole suite: a mutation is
*killed* when a test fails, and *survives* when every test still passes, which means a rule no test checks. It exits
1 if any mutation survives or no longer applies; `--update` records the tests that killed each one.

```sh
# Linux (or the gatelink-dev container, as above, with the repo mounted read-write or a copy):
python3 tools/mutate.py --libs /libs --jobs 4                 # all of them: a few minutes per mutation per job
python3 tools/mutate.py --invariant interlock --libs /libs     # one invariant
python3 tools/mutate.py --list                                # the catalog
make -C tests/native run JOBS=4 T=interlock                   # the system tests in 4 parallel shards (T= filters)
```

`tests/tools/test_mutate.py` (CI) checks that every mutation still applies to the firmware as it is, so a firmware
change that moves a mutation's text fails CI until the catalog follows: update its `find`/`replace` to break the same
rule in the new code, or remove it if the rule went with the change. A new invariant, or a new way to break one that
tests miss, gets an entry here with a test that kills it.
