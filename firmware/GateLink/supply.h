#pragma once
#include <Arduino.h>

// The board's own supply (VIN), as the MKR WAN 1310's BQ24195L charger / power path sees it (I2C 0x6B; read-only,
// its registers are never written). Power good drops as soon as VIN sags, before the board moves to its LiPo:
// on the bench the house's dropped ~0.2 s before the Shelly's relay on a 12 V cut, and on 300 ms dips the IN2 opto
// never noticed (role_house.cpp, ctrl_power_pmic).
void supplyBegin();
void supplyPoll(uint32_t now);  // every few ms; logs `supply` (a = power good, b = REG08) on a change
bool supplyKnown();             // the charger answered at boot
bool supplyGood();              // VIN power good (true if the charger didn't answer)
