# Fuzzing the firmware's parsers

libFuzzer targets for everything the board reads from outside: radio frames (`link.cpp` and the roles' handlers),
the JSON console (`console.cpp`) and the config record in the SPI flash (`config.cpp`). Each target builds the real
firmware (`app link role_gate role_house io log history console config`, plus Crypto) with AddressSanitizer and
UBSan, runs it on a fake board ([hal.cpp](hal.cpp)), and checks behavioural invariants as it goes, so a
memory-safety bug or a breach under hostile input crashes with a reproducer.

The committed corpus doubles as a regression test: `make -C tests/native` (CI) replays it with plain g++, no clang
needed.

## Targets

| Target | Input | Reaches |
|---|---|---|
| `fuzz_frames` | Byte 0 picks the board (house or gate; warm or cold boot; clock near the `millis()` wrap), then records: authenticated frames of any type and content, raw bytes, replays, peer restarts, input changes, console commands, radio faults, time (format at the top of [fuzz_frames.cpp](fuzz_frames.cpp)) | `handleFrame` and the replay window, HELLO/HELLO_ACK and the held command, `handleStatus`, `handleCmd`, `handleCfgSet`, DIAG, PING/PONG, ACKs, the house's controller sync |
| `fuzz_console` | Byte 0 picks the board (house, gate, blank, gate near the wrap) and port (USB, or the UART, where requests need a CRC; or the harness signs each line), then bytes, a line at a time ([fuzz_console.cpp](fuzz_console.cpp)) | Every console command, the CRC check, line limits, held requests |
| `fuzz_config` | The SPI flash, sectors 0–3 from address 0 (padded with erased bytes, cut at 16 KB) ([fuzz_config.cpp](fuzz_config.cpp)) | `configLoad` (record decoder), `configCountBoot`, `configSave`, `configSaveParam`, `configSaveKey`, `configFactoryReset` |

The board starts provisioned: the harness runs the real firmware once (role set, the key `FUZZ_KEY`,
`configSave`) and keeps that flash image. In `fuzz_frames` the harness plays the other board with a session of its
own: it completes the real handshake (answers the board's HELLO challenge with a HELLO_ACK) and MACs its frames
with HMAC-SHA256 truncated to 8 bytes, as `link.cpp` does, using the board's current key and net id. A "warm" board
is up, verified, settled (the gate has sent its first STATUS, ACKed; the house has had a STATUS and is armed); a
"cold" one has only just booted.

Checksums the fuzzer can't find by mutation are fixed up by the harness: frame MACs (always), the console CRC
(`fuzz_frames` templates with the crc flag, `fuzz_console` byte 0 bit 3) and config record CRCs (`fuzz_config`,
unless flash byte 256, which the firmware never reads, is even).

## Invariants checked

On every loop pass and at every relay edge (hal.cpp), whatever the input:

- Gate K1 and K2 are never both energized, and neither goes on within 100 ms (`INTERLOCK_MS`) of the other releasing.
- A gate relay is on no longer than `pulse_ms` + 50 ms, or a console `relay.test`'s `ms` + 50 ms when one asked
  for it just before (read as console.cpp reads it, and counted only if its reply says it ran: a refused one, bad
  crc or line too long, doesn't count). A relay pulsed again while on (a second command the same way) is measured
  from that pulse's start, and a second request in the very millisecond it went on (a delayed start, then a
  `relay.test` read in the same loop pass) allows the longer of the two; `GATELINK_FUZZ_STRICT_PULSE=1` measures
  from the first pulse (see Findings). Loop stalls count: the fake board blocks as long as the real one (flash
  erase 45 ms, radio init 451 ms: `LoRa.begin()` on the MKR WAN 1310 waits 200 + 200 + 50 ms), so a stall that
  straddles the end of a pulse shows up as a longer pulse.
- House K2 (the alarm's contact sensor, `sensor_invert` 0) reads closed only while the house knows the gate as
  closed (its last `gate_state`) and, with `linkloss_open`, its link is up (`link_up`/`link_down`); a console
  `relay.test` of K2 that ran overrides it until K2 next releases.
- No loop pass blocks for the 8 s watchdog.
- Every line the console prints is one JSON object: a reply (`ok`) or an event.
- `fuzz_config`: every loaded setting is in range (`paramValid`); the boot counter counts up; save then load gives
  the same config; `configSaveParam` writes one setting and leaves an unsaved edit unsaved; `configSaveKey` keeps
  the settings; nothing loads after `configFactoryReset`; none of the saves fails on a healthy chip.

Plus everything ASan and UBSan catch (`-fno-sanitize-recover`).

## Fresh firmware state per input

The firmware keeps its state in globals and statics, and libFuzzer runs every input in one process. The libFuzzer
build force-includes [sections.h](sections.h) into each firmware file: `#pragma clang section bss="gl_bss"
data="gl_data"` puts all of their globals and statics (function-local ones too) into two sections. The harness
copies `__start_gl_bss`..`__stop_gl_bss` and `__start_gl_data`..`__stop_gl_data` after static initialisation and
copies them back before every input (word by word, without ASan's checks: the sections hold ASan's redzones, which
stay in force; the pragma sets a section attribute, so ASan still instruments these globals). The fake board
(`Hal`) and the peer are plain copies. Warm boards are booted once per role at start-up and restored the same way.

This is verified three ways:

- `check_sections.sh` (part of `build-fuzz`) fails the build if any firmware object still defines a variable in
  `.bss`/`.data` (other than the sanitizers' own bookkeeping).
- At start-up each fuzzer checks that the firmware's globals lie in the sections, that two warm boots from pristine
  RAM give identical RAM and output, that an input gives the same output (console lines and frames, hashed)
  before and after another input ran, and that a setting the second input changed is back for the next. It prints
  `gatelink-fuzz: fresh firmware RAM per input verified (...)`.
- The g++ replay build has no such sections: [replay_main.cpp](replay_main.cpp) runs each input in a `fork()`ed
  child of a process that never ran firmware code (it provisions in a child of its own), and the harness aborts
  if a process is asked to run a second input.

## Running

Fuzzing needs clang with libFuzzer (Ubuntu 24.04: `clang-18 libclang-rt-18-dev`). On Windows use the `gatelink-dev`
Docker image (Ubuntu 24.04 with g++ 13 and clang 18), from the repo root in Git Bash:

```sh
LIBS="$(cd ~/Documents/Arduino/libraries && pwd -W)"
MSYS_NO_PATHCONV=1 docker run --rm --security-opt seccomp=unconfined -v "$(pwd -W):/src" -v "$LIBS:/libs:ro" \
  -v gatelink-build:/build -w /src/tests/native gatelink-dev \
  make -f fuzz/Makefile LIBS=/libs BUILD=/build fuzz-smoke
```

| Goal (`make -f fuzz/Makefile ...`, from `tests/native`) | |
|---|---|
| `replay` | g++ build; every input in `corpus/<target>` must run clean, every one in `crashes/<target>` must still crash. Part of `make -C tests/native` |
| `build-fuzz` | the three libFuzzer binaries in `$(BUILD)/fuzz/`, and the section check |
| `fuzz-smoke` | each target for `FUZZ_SECONDS` (default 60) from its corpus; new inputs go to `$(BUILD)/fuzz/work/<target>`, crashes to `$(BUILD)/fuzz/artifacts/`. `TARGETS=fuzz_config` runs one; `FUZZ_FLAGS` adds libFuzzer flags (e.g. `-jobs=4 -workers=4`) |
| `corpus-merge` | minimise what fuzzing found (`work/`) into `corpus/<target>`: only inputs that add coverage |

A single binary takes the usual libFuzzer flags, e.g. `$(BUILD)/fuzz/fuzz_frames -max_total_time=600 -max_len=4096
work/ fuzz/corpus/fuzz_frames`. Use `-max_len` 4096 (frames), 8192 (console, with `-dict=fuzz/console.dict`) and
16384 (config, with `-dict=fuzz/config.dict`), as `fuzz-smoke` does.

**CI.** `make -C tests/native` in the CI workflow replays the corpus and the known crashes. The fuzz workflow
(`.github/workflows/fuzz.yml`) runs `fuzz-smoke` for 60 s per target on pull requests that touch the firmware or
these tests, and for 30 minutes per target nightly; crashes and the new inputs are uploaded as artifacts. Merge
useful inputs back with `corpus-merge` (keep the corpus small: tens of files).

## Seeds

The corpus is written by [make_seeds.py](make_seeds.py) (no `.bin` extension: `.gitignore` drops those):

```sh
GATELINK_FUZZ_DUMP_IMAGES=/tmp/img build/fuzz/replay_config /dev/null   # the provisioned flash images
python tests/native/fuzz/make_seeds.py --images /tmp/img
```

Add a seed there (a scenario reads as a list of records: `auth(CMD, cmd(1, OPEN)) + wait(1000) + ack(STATUS)`),
rerun it, and check it does what you meant with `GATELINK_FUZZ_TRACE=1` (below). Inputs a fuzzing run found go in
with `corpus-merge`, and an input that once tripped a harness bug stays as a regression seed under a descriptive
name (`corpus/fuzz_console/relay-tests-one-refused-by-crc`: the relay monitor took a refused `relay.test` for the
one that ran; `relay-test-cmd-ending-in-nul`: `"cmd":"relay.test\u0000"` runs as `relay.test`, since console.cpp
reads the command as a C string, and the monitor must read it the same way; in `make_seeds.py`,
`corpus/fuzz_frames/gate-relay-test-refused-after-one-that-waits`: refused requests pushed out the one that was
waiting out the interlock; `gate-relay-test-as-a-delayed-pulse-starts`: a second request in the millisecond the
relay went on was ignored).

## Reproducing a crash

libFuzzer writes the input to `$(BUILD)/fuzz/artifacts/<target>-crash-<sha1>` and prints the stack. Then:

```sh
GATELINK_FUZZ_TRACE=1 build/fuzz/fuzz_frames <crash file>       # what the board did: console lines, frames, relays
build/fuzz/replay_frames -v <crash file>                         # the g++ build, in a child process
build/fuzz/fuzz_frames -minimize_crash=1 -max_total_time=60 -exact_artifact_path=min <crash file>
```

An invariant breach prints `==GATELINK INVARIANT==` and what broke, then traps. Decide what it is: a harness bug
(fix the harness), a check that is wrong (fix the check, explain it here), or a firmware bug. For a firmware bug
that isn't fixed in the same change: put the minimised input in `crashes/<target>/` with a name that says what it
is, make the harness tolerate exactly that case unless `GATELINK_FUZZ_KNOWN_BUGS` is set (so fuzzing can find
other bugs), add it to Known bugs below and to `TODO.md`. `replay` runs `crashes/` with the variable set, so each
reproducer must keep crashing on an invariant (`==GATELINK INVARIANT==`; crashing some other way fails the run);
the fix makes it stop, which fails the run until the input moves to the corpus and the tolerance goes.

| Variable | Effect |
|---|---|
| `GATELINK_FUZZ_TRACE=1` | Print what happens, stamped with the board's `millis()` |
| `GATELINK_FUZZ_KNOWN_BUGS=1` | Also trap on the known bugs below |
| `GATELINK_FUZZ_STRICT_PULSE=1` | Measure a re-pulsed relay from its first pulse |
| `GATELINK_FUZZ_DUMP_IMAGES=<dir>` | Write the provisioned flash images (`house.bin`, `gate.bin`) at start-up |

## Known bugs

Found by these targets, reported, not fixed here; their reproducers are in `crashes/`.

- **The boot counter falls back to 1 after a slot reads high** (`crashes/fuzz_config/boot-counter-wraps-to-1`: a slot
  of 0xFFFFFFFD; `crashes/fuzz_config/boot-counter-high-slot-then-1`: a torn slot of 3392943128, found by CI's first run).
  `configCountBoot` returns the largest slot value + 1 and maps 0xFFFFFFFF to 1; the large slot stays (the sector
  holding the largest value is never erased), so every later boot counts 1 again. A slot can read that high when a
  program is cut short (it "can only read high") or a read is garbled. The count seeds the session id, which must
  never repeat under one key.
- **A console `reboot` during a gate relay pulse holds the relay up to 100 ms long**
  (`crashes/fuzz_frames/console-reboot-during-pulse`: `relay.test` 500 ms, `reboot` 25 ms before its end: K1 on
  575 ms). `reboot` flushes its reply and `delay(100)`s before resetting, and isn't held while a relay pulses as
  `config.save` and the others are.
- **A radio fault or retry during a gate relay pulse holds the relay up to ~0.45 s long**
  (`crashes/fuzz_frames/radio-fault-during-pulse`: `relay.test` 500 ms, a radio reset 200 ms in: K1 on 656 ms).
  radio.cpp re-initialises the radio (`fault()`: a TX that never finished, a reset seen in RX; and the retry every
  5 s while it doesn't answer) whenever it's due, and `LoRa.begin()` blocks the loop ~450 ms, against "nothing
  that stops the loop (a radio restart) may run while a pulse does". A supply dip from the relay coil switching on
  while the gate sends the command's ACK is the likely way in. hal.cpp excuses the stall of those two paths
  (`radioInit(true)`) unless `GATELINK_FUZZ_KNOWN_BUGS` is set.

## Findings that aren't bugs

- **A second command the same way restarts the pulse** (`corpus/fuzz_frames/gate-cmd-same-direction-twice`, which
  crashes with `GATELINK_FUZZ_STRICT_PULSE=1`): two OPENs with different ids 277 ms apart keep K1 on 777 ms, each
  pulse `pulse_ms` from its own start. `Relay::pulse` on a relay that is on extends it; it's still a pulse per
  command, so the default check measures each pulse from its start. A stream of same-direction commands less than
  `pulse_ms` apart keeps the relay on for as long as it lasts.
