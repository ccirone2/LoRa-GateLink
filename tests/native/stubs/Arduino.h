// Host stand-in for the Arduino core: just what link.cpp, config.cpp and their headers use. Time is the test's
// (sim.cpp), and random() is a seeded PRNG, so every run is the same.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);
