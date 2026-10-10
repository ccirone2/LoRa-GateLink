// Host stand-in for the Arduino core: just what the firmware's portable files and their headers use. The test
// program defines the functions: time is the test's, random() a seeded PRNG, and pins belong to the simulated site
// (world.cpp), so every run is the same.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);

#define LOW 0
#define HIGH 1
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define INPUT_PULLDOWN 0x3
void pinMode(uint32_t pin, uint32_t mode);
int digitalRead(uint32_t pin);
void digitalWrite(uint32_t pin, uint32_t value);
void analogWrite(uint32_t pin, int value);

// MKR WAN 1310 pin numbers (variant.h)
#define LED_BUILTIN 6
#define A0 15
#define A1 16
#define A2 17
#define A3 18
#define A4 19
#define A5 20
#define A6 21

// SAMD21 PM->RCAUSE bits
#define PM_RCAUSE_POR 0x01
#define PM_RCAUSE_BOD12 0x02
#define PM_RCAUSE_BOD33 0x04
#define PM_RCAUSE_EXT 0x10
#define PM_RCAUSE_WDT 0x20
#define PM_RCAUSE_SYST 0x40
