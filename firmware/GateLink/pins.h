// Pin map for MKR WAN 1310 on an MKR Relay Proto Shield (TSX00003).
// Verify against the shield silkscreen before wiring.
#pragma once

// Relays on the MKR Relay Proto Shield.
#define PIN_K1 1
#define PIN_K2 2

// Inputs: internal pull-down, active = HIGH (3.3 V from a PNP opto output or a contact to 3.3 V).
// Never drive above 3.3 V. Use screw terminals / proto area.
#define PIN_IN1 A1
#define PIN_IN2 A2
// IN3/IN4: read and reported. Gate IN3 = AC power sense (power_sense); the rest are spare.
#define PIN_IN3 A3
#define PIN_IN4 A4

// Serial1 (pins 13 RX / 14 TX): optional second console (uart_console), a 3.3 V USB-to-UART adapter on the bench.

// Role usage:
//   HOUSE: IN1 = Shelly Wave 1 relay (O/I) "switch state"
//          IN2 = controller (Shelly) supply via a PNP opto (ctrl_power_sense)
//          IN3-IN4 = spare (read and logged only)
//          K1  = Shelly SW input (state sync: energized = gate not closed)
//          K2  = 2GIG wireless contact sensor terminals (energized = gate closed)
//   GATE:  (all inputs via a PNP-output opto board)
//          IN1 = CSW24UL AUX relay "open limit"
//          IN2 = CSW24UL AUX relay "closed limit"
//          IN3 = AC power: the 24 V supply on mains (power sense)
//          K1  = CSW24UL OPEN + COM   (pulsed only)
//          K2  = CSW24UL CLOSE + COM  (pulsed only)
