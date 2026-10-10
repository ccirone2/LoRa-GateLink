// fuzz_console: bytes into the JSON console (console.cpp) of a running board, a line at a time with the loop running
// in between, under ASan/UBSan and the monitors in hal.cpp: every line the board prints must be a JSON reply or
// event, and the gate's relays keep their invariants whatever the console asks (relay.test, saves while pulsing...).
//
// Input: byte 0, then the bytes for the port.
//   byte 0: bits 0-1 board (0 house, 1 gate: both warm, verified with the harness's peer; 2 blank, role unset;
//           3 gate starting ~30 s before millis() wraps), bit 2 the UART console (uart_console on; every request
//           needs its crc there) instead of USB, bit 3 the harness signs each line that ends in '}' (appends the
//           crc member, which the fuzzer can't compute itself)
//   then: fed to the port line by line ('\n' ends a line), the board running until it has read each one.
#include <string>
#include "harness.h"
#include "config.h"
#include "console_io.h"

static int runInput(const uint8_t *data, size_t size) {
  if (!size) return 0;
  uint8_t head = data[0];
  int port = (head & 4) ? CON_UART : CON_USB;
  try {
    switch (head & 3) {
      case 0: startWarm(FR_HOUSE, false); break;
      case 1: startWarm(FR_GATE, false); break;
      case 2:
        startBlank(1000);
        boot(PM_RCAUSE_POR);
        break;
      default: startWarm(FR_GATE, true); break;
    }
    if (port == CON_UART) {
      cfg.uart_console = 1;  // as config.set would; consolePoll starts the port
      pass(1);
    }
    bool sign = head & 8;
    size_t i = 1;
    while (i < size && budgetLeft()) {
      size_t j = i;
      while (j < size && data[j] != '\n') j++;
      std::string line((const char *)data + i, j - i);
      if (sign) line = withCrc(line);
      for (char c : line) hal.rx[port].push_back((uint8_t)c);
      if (j < size) hal.rx[port].push_back('\n');
      i = j + 1;
      if (!settle()) break;
    }
    finish();
  } catch (const BoardReset &) {
    // reboot: the input ends here
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return runInput(data, size);
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  std::string a = std::string(1, '\x01') + "{\"id\":1,\"cmd\":\"status\"}\n{\"id\":2,\"cmd\":\"relay.test\",\"k\":1}\n";
  std::string b = std::string(1, '\x01') + "{\"id\":3,\"cmd\":\"config.set\",\"params\":{\"pulse_ms\":1234}}\n";
  harnessSetup();
  harnessSelfTest(runInput, Bytes(a.begin(), a.end()), Bytes(b.begin(), b.end()));
  return 0;
}
