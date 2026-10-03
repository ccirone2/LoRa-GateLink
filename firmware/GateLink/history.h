#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Link quality history: a RAM ring of buckets (default one hour, four days deep) numbered from boot or the last
// hist.clear, so it needs no clock. Counters are the increase of the link/radio totals during the bucket; levels
// (RSSI, SNR, noise) are kept as min/sum/count. Diagnostics only: nothing here may affect commands, sync or the
// outputs. Lost on every reset (reboots are in the log).
// Closed buckets kept, besides the one in progress (64 B each). Bound by RAM: a config.get reply takes ~5 KB of
// heap, and a week (168) left under 1 KB between heap and stack.
#define HIST_DEPTH 96
#define HIST_PERIOD_S 3600  // default bucket length; hist.clear can set 60..3600 until the next boot
#define HIST_PAGE 12        // buckets per hist.get reply: keeps the reply's JSON well inside the heap
#define NOISE_NONE INT16_MIN

// What the gate reports about its side in each STATUS (house only).
struct PeerReport {
  int16_t rssi;  // at the gate, of its last frame from the house (0 = none yet)
  int8_t snr;
  bool ext;      // the fields below were in the STATUS (gate firmware 0.4.0 on)
  uint16_t retries, giveups, crcErr;  // running totals, low 16 bits; going down means its link restarted
  int8_t noiseAvg, noiseMax;  // dBm since its previous STATUS (0 = no sample)
};

void histBegin();
void histPoll(uint32_t now, bool linkUp);
void histPeer(const PeerReport &r);
bool histClear(uint32_t periodS);  // false if periodS is out of range
uint32_t histPeriod();
void histGet(JsonObject res, int32_t from, int32_t n);
int16_t histNoiseNow();  // smoothed noise floor, dBm (NOISE_NONE before the first sample)
// Noise since the previous call, for the gate's STATUS: average and peak, dBm (0 = no sample).
void histNoiseTake(int8_t &avg, int8_t &max);
