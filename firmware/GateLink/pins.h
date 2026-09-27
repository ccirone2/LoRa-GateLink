// Pin map for MKR WAN 1310 on an MKR Relay Proto Shield (TSX00003).
// Verify against the shield silkscreen before wiring.
#pragma once

// Relays on the MKR Relay Proto Shield.
#define PIN_K1 1
#define PIN_K2 2

// Dry-contact inputs (contact to GND, internal pull-up). Use screw terminals / proto area.
#define PIN_IN1 A1
#define PIN_IN2 A2

// Role usage:
//   HOUSE: IN1 = Shelly Wave 1 relay (O/I) "switch state"
//          K1  = Shelly SW input (state sync: energized = gate not closed)
//          K2  = 2GIG wireless contact sensor terminals (energized = gate closed)
//   GATE:  IN1 = CSW24UL AUX relay "open limit"
//          IN2 = CSW24UL AUX relay "close limit"
//          K1  = CSW24UL OPEN + COM   (pulsed only)
//          K2  = CSW24UL CLOSE + COM  (pulsed only)
