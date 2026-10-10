#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Link quality history: a RAM ring of buckets (default one hour, four days deep) numbered from the last hist.clear,
// so it needs no clock. Counters are the increase of the link/radio totals during the bucket; levels (RSSI, SNR,
// noise) are kept as min/sum/count. Diagnostics only: nothing here may affect commands, sync or the outputs.
// Each bucket completed is also written to the SPI flash (histlog.h), and a boot loads the newest back, so the history
// survives resets but for the bucket in progress (each bucket carries the boot it was recorded in). The numbering goes
// on from the newest, as if the history had run without a break: a reader can't tell how long a reset took.
// Closed buckets kept, besides the one in progress (68 B each). Bound by RAM: a config.get reply takes ~5 KB of
// heap, and a week (168) left under 1 KB between heap and stack.
#define HIST_DEPTH 96
#define HIST_PERIOD_S 3600  // bucket length without a history in the flash; hist.clear can set 60..3600 (kept with it)
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

// At boot, the config loaded: loads the history kept in the flash. bootCount: this boot's (appBootCount), stamped on
// its buckets.
void histBegin(uint32_t bootCount);
void histPoll(uint32_t now, bool linkUp);
void histPeer(const PeerReport &r);
// A completed bucket is waiting to be written to the flash. histSave() writes it (one per call, each once, written or
// not): it stops the loop ~0.5 s and takes the radio off the air, so the app calls it only while no relay pulses.
bool histSaveDue();
void histSave();
enum HistClear : uint8_t { HIST_CLEARED, HIST_BAD_PERIOD, HIST_NOT_SAVED };
// Empties the history and starts it again at bucket 0 with buckets of periodS, in the flash too (HIST_NOT_SAVED: that
// write failed, so a reset would bring the old history back). Writes to the flash, as histSave() does.
HistClear histClear(uint32_t periodS);
uint32_t histPeriod();
void histGet(JsonObject res, int32_t from, int32_t n);
int16_t histNoiseNow();  // smoothed noise floor, dBm (NOISE_NONE before the first sample)
// Noise since the previous call, for the gate's STATUS: average and peak, dBm (0 = no sample).
void histNoiseTake(int8_t &avg, int8_t &max);
