# Host unit tests: link and config

`link.cpp` and `config.cpp` built with g++ and run on the PC. These tests cover paths the bench can't reach, or
can't reach reliably:

- replays across sessions and restarts
- HELLO side effects
- retry spacing
- timer wraps
- power cuts mid-save
- records from other firmware

Nothing here talks to hardware; the bench suite in `tests/e2e` remains the end-to-end check.

```sh
make -C tests/native CRYPTO=<rweather Crypto library>/src       # builds and runs; exit status 1 on a failure
make -C tests/native CRYPTO=... build/tests && tests/native/build/tests retries   # one test (name substring)
```

`CRYPTO` defaults to `~/Arduino/libraries/Crypto/src`, where `arduino-cli` installs it on Linux (CI). On
Windows it's `~/Documents/Arduino/libraries/Crypto/src`, and with no g++ installed you can run the tests in
Docker:

```sh
MSYS_NO_PATHCONV=1 docker run --rm --security-opt seccomp=unconfined -v "$(pwd -W):/src" \
  -v "$(cd ~/Documents/Arduino/libraries/Crypto/src && pwd -W):/crypto:ro" \
  -w /src/tests/native node:20 make CRYPTO=/crypto
```

They build with AddressSanitizer and UBSan, and with `-Werror` for everything except the Crypto library. `make`
runs them with address randomization off (`setarch -R`) when it can: under kernels with `vm.mmap_rnd_bits` = 32,
such as Docker Desktop's, GCC 12's ASan sometimes hangs at startup printing `AddressSanitizer:DEADLYSIGNAL`. In
Docker, `setarch -R` needs `--security-opt seccomp=unconfined`.

## How it works

- **`stubs/`** stands in for the Arduino core and FlashStorage. `millis()` is the test's clock (`simNow`), and
  `random()` is seeded, so every run is the same.
- **`sim.cpp`** holds the fakes:
  - The radio (`radio.h`): one SX127x per node on a shared channel. It models LoRa airtime at the node's
    SF/BW/CR, half duplex (a node can't hear while it transmits), and a channel that reads busy a few symbols
    into the other node's frame. `Sim::drop` decides per frame whether it gets through, and `Sim::inject`
    plays a recorded frame into a receiver.
  - The SPI NOR flash (`extflash.h`, sectors 0–3). Programming only clears bits. `cutNextProgram` simulates a
    power cut mid-write; `garbleReads` simulates a garbled bus.
  - `logEvent`, which records events per node.
- **Two nodes, one `link.cpp`.** `link.cpp` keeps its state in file-scope statics, so `link_house.cpp` and
  `link_gate.cpp` compile it into two namespaces (`link_node.inc`). Each `Node` calls its own copy through a
  `LinkApi`.
  - `Node::Ctx` swaps the node's `Config` into the global `cfg` (and `activeRole`) for the duration of each
    call, and points the fakes at the node.
  - `link_types.h` includes `link.h` with its function names renamed. Otherwise argument-dependent lookup
    would make the namespaced calls inside `link.cpp` ambiguous.
- **`config.cpp`** is compiled once and tested on its own; no node is current.
- **Framework.** A test is `TEST(name) { ... }` with `CHECK`, `CHECK_EQ` and `CHECK_IN`. `XFAIL_TEST(name,
  reason)` marks a known bug (an open REVIEW/TODO item): the test must fail, and the run fails once it passes,
  so whoever fixes the bug flips it to `TEST`. `Sim::dumpAir()` prints every frame when a link test needs
  explaining.

## Adding tests

- Link: build a `Sim`, call `s.handshake()`, then drive the two `Node`s:
  - `sendReliable`, `send`, `ack`, `ackLater`, and `begin` (a restart)
  - `onRx`, to answer the way a role would (by default reliable messages are ACKed `RES_OK`)
  - `drop` and `inject` for the channel

  Then check `stats()`, `rx`, `acks`, `logs` and `s.sent(node, type)`.
- Config: call `fresh()` first (blank flash, defaults), then change `cfg`, save, load and inspect `flash.mem`.
