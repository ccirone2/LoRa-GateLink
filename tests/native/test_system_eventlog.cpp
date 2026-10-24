// The event log across resets (log.cpp, evlog.cpp; ROADMAP F1): the RAM ring kept through every reset but power-on,
// written to the flash journal in batches (14 entries, or the oldest waiting 10 min; at most one write a minute, never
// while a relay pulses), read back with log.get rec; floods coalesced into one entry. Through the whole simulated site
// (world.h).
#include <Arduino.h>
#include <string>
#include "world.h"

namespace {

const uint32_t ELOG_BASE = 20 * 4096, ELOG_END = 36 * 4096;  // evlog.h ELOG_SECTOR0, ELOG_SECTORS
const uint32_t WAIT_MS = 600000, GAP_MS = 60000;            // log.h LOG_SAVE_WAIT_MS, LOG_SAVE_GAP_MS

struct Site : World {
  using World::World;
  ~Site() noexcept(false) {
    if (violations.empty()) return;
    std::string all = "invariant monitors:";
    for (const std::string &v : violations) all += "\n        " + v;
    violations.clear();
    if (std::uncaught_exceptions()) {
      printf("      %s\n", all.c_str());
      return;
    }
    throw Failure(all);
  }
};

// Programs into the journal's sectors on b since flashOps index `from`.
std::vector<Board::FlashOp> journalWrites(const Board &b, size_t from = 0) {
  std::vector<Board::FlashOp> out;
  for (size_t i = from; i < b.flashOps.size(); i++)
    if (b.flashOps[i].op == 'P' && b.flashOps[i].addr >= ELOG_BASE && b.flashOps[i].addr < ELOG_END)
      out.push_back(b.flashOps[i]);
  return out;
}

uint32_t bootCount(Board &b) {
  return b.request("info")["boot_count"];
}

struct Ev {
  std::string ev;
  uint32_t t;
  int32_t a, b;
  int n;     // times it came (1, or the coalesced count)
  int boot;  // -1: this boot
};

std::vector<Ev> entries(const JsonDocument &r) {
  std::vector<Ev> out;
  for (JsonVariantConst e : r["log"].as<JsonArrayConst>())
    out.push_back(Ev{ e["ev"].as<std::string>(), e["t"], e["a"], e["b"], e["n"] | 1, e["boot"] | -1 });
  return out;
}

std::vector<Ev> ring(Board &b) {
  JsonDocument r = b.request("log.get");
  CHECK(r["ok"] == true);
  return entries(r);
}

// Every record in the journal, followed back by the cursor as the web console does, flattened oldest entry first.
std::vector<Ev> journal(Board &b) {
  std::vector<Ev> out;
  uint32_t before = UINT32_MAX;
  for (;;) {
    JsonDocument r = b.request("log.get", "\"before\":" + std::to_string(before));
    CHECK(r["ok"] == true);
    std::vector<Ev> e = entries(r);
    if (e.empty()) {
      CHECK(r["seq"].isNull());
      return out;
    }
    uint32_t seq = r["seq"];
    CHECK(seq < before);
    before = seq;
    out.insert(out.begin(), e.begin(), e.end());
  }
}

// v holds entry e (as logged), from boot `boot` (-1: this one).
bool has(const std::vector<Ev> &v, const Ev &e, int boot) {
  for (const Ev &x : v)
    if (x.ev == e.ev && x.t == e.t && x.a == e.a && x.b == e.b && x.boot == boot) return true;
  return false;
}

int countEv(const std::vector<Ev> &v, const std::string &ev, int boot = -2) {
  int n = 0;
  for (const Ev &e : v)
    if (e.ev == ev && (boot == -2 || e.boot == boot)) n++;
  return n;
}

}  // namespace

// [event-log] A gate move, then a software reset of each board: the newest events from before it are still in the ring
// after the boot (with that boot's number), and are written to the journal at once, so a later power cut keeps them.
TEST(event_log_ring_survives_a_reset_and_goes_to_the_journal_at_once) {
  for (int which : { 0, 1 }) {
    Site w;
    w.commission();
    Board &b = w.board(which);
    w.user(true);
    CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 20000));
    w.run(3000);
    uint32_t boot0 = bootCount(b);
    std::vector<Ev> before = ring(b);
    CHECK(!before.empty());
    Ev last = before.back();
    size_t ops = b.flashOps.size();
    b.reset(PM_RCAUSE_SYST);
    CHECK(w.runUntil([&] { return b.running(); }, 2000));
    w.run(3000);
    CHECK_EQ(bootCount(b), boot0 + 1);
    std::vector<Ev> after = ring(b);
    CHECK(has(after, last, (int)boot0));
    CHECK(countEv(after, "gate_state", (int)boot0) >= 1);
    CHECK(!journalWrites(b, ops).empty());  // the boot before's unsaved entries, written at once
    b.powerOff();
    w.run(1000);
    b.powerOn();
    CHECK(w.runUntil([&] { return b.running(); }, 2000));
    w.run(3000);
    CHECK_EQ(countEv(ring(b), "gate_state", (int)boot0), 0);  // the RAM after a power-on: nothing kept
    CHECK(has(journal(b), last, (int)boot0));
  }
}

// [event-log] The ring reply counts what isn't in the journal yet and this boot's journal writes (0.20.1, TODO.md: a
// boot that writes nothing): after a reset the boot before's unsaved entries go out at once, and a failed write is
// counted (its entries stay in the ring only).
TEST(event_log_ring_reply_counts_unsaved_entries_and_journal_writes) {
  Site w;
  w.commission();
  Board &b = w.board(1);
  w.user(true);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 20000));
  w.run(3000);
  JsonDocument r = b.request("log.get");
  CHECK(r["unsaved"].as<int>() > 0);  // the move, waiting for a record's worth or 10 min
  b.reset(PM_RCAUSE_SYST);
  CHECK(w.runUntil([&] { return b.running(); }, 2000));
  w.run(3000);
  r = b.request("log.get");
  CHECK_EQ(r["saves"].as<int>(), 1);  // counted from this boot
  CHECK_EQ(r["save_fails"].as<int>(), 0);
  CHECK(r["unsaved"].as<int>() <= 2);  // only what came after the write (the session)
  w.user(false);  // something unsaved again, written once it has waited 10 min
  CHECK(w.runUntil([&] { return w.houseSees() == GS_CLOSED_; }, 20000));
  b.failPrograms = 1;
  w.run(WAIT_MS + 5000);
  r = b.request("log.get");
  CHECK_EQ(r["saves"].as<int>(), 2);
  CHECK_EQ(r["save_fails"].as<int>(), 1);
  CHECK_EQ(r["unsaved"].as<int>(), 0);  // not retried: those are in the ring only
}

// [event-log] The ring reply is streamed an entry at a time (0.19.1: as one document, a ring of earlier-boot entries
// took ~1 KB of heap more than config.get): a full ring from the boot before, coalesced floods among it, still comes
// back whole and in order, with the request's id, `now` and `boot`.
TEST(event_log_full_ring_from_the_boot_before_reads_back_whole) {
  const size_t RING = 64;  // LOG_SIZE
  Site w;
  w.commission();
  for (int i = 0; i < 16; i++) {
    w.user(i % 2 == 0);
    CHECK(w.runUntil([&] { return w.houseSees() == (i % 2 == 0 ? GS_OPEN_ : GS_CLOSED_); }, 20000));
    w.run(10000);  // past the sync window the move opened
  }
  for (int i = 0; i < 3; i++) {  // replays at the gate: one entry, n 3
    CHECK(w.house.request("debug.replay")["sent"] == true);
    w.run(500);
  }
  uint32_t boot0 = bootCount(w.gate);
  std::vector<Ev> before = ring(w.gate);
  CHECK_EQ(before.size(), (size_t)RING);
  w.gate.reset(PM_RCAUSE_SYST);
  CHECK(w.runUntil([&] { return w.gate.running(); }, 2000));
  w.run(3000);
  JsonDocument r = w.gate.request("log.get");
  CHECK(r["ok"] == true);
  CHECK(!r["id"].isNull());
  CHECK_EQ(r["boot"].as<uint32_t>(), boot0 + 1);
  CHECK(r["now"].as<uint32_t>() > 0);
  std::vector<Ev> after = entries(r);
  CHECK_EQ(after.size(), (size_t)RING);
  // The boot before's entries, as they were, in order, then this boot's.
  size_t k = 0;
  while (k < after.size() && after[k].boot == (int)boot0) k++;
  CHECK(k > RING / 2);
  if (k == 0) return;
  CHECK(after[k - 1].ev == "replay" && after[k - 1].n == 3);
  for (size_t i = 0; i < k; i++) {
    const Ev &x = after[i], &y = before[before.size() - k + i];
    CHECK(x.ev == y.ev && x.t == y.t && x.a == y.a && x.b == y.b && x.n == y.n);
  }
  for (size_t i = k; i < after.size(); i++) CHECK_EQ(after[i].boot, -1);
}

// [event-log] The streamed ring reply echoes a request id of any length (a 300-character string), whole: one line that
// parses, with the id and the entries.
TEST(event_log_ring_reply_echoes_a_long_id) {
  Site w;
  w.commission();
  std::string id(300, 'x');
  size_t lines0 = w.gate.lines.size();
  auto reply = [&] {
    for (size_t i = lines0; i < w.gate.lines.size(); i++)
      if (w.gate.lines[i].find("\"log\":[") != std::string::npos) return w.gate.lines[i];
    return std::string();
  };
  w.gate.send("{\"id\":\"" + id + "\",\"cmd\":\"log.get\"}");
  CHECK(w.runUntil([&] { return !reply().empty(); }, 1000));
  JsonDocument r;
  CHECK(!deserializeJson(r, reply()));
  CHECK(r["id"] == id);
  CHECK(r["ok"] == true);
  CHECK(r["log"].size() > 0);
  CHECK(!r["now"].isNull() && !r["boot"].isNull());
}

// [event-log] A power cut loses only what hadn't been written: the oldest unsaved entry waits at most 10 min.
TEST(event_log_power_cut_loses_only_the_unsaved_entries) {
  Site w;
  w.commission();
  Board &b = w.gate;
  uint32_t boot0 = bootCount(b);
  size_t ops = b.flashOps.size();
  w.run(WAIT_MS + 4 * GAP_MS);  // the oldest waited 10 min; the rest follow a minute apart
  CHECK(!journalWrites(b, ops).empty());
  size_t ops1 = b.flashOps.size();
  w.extPress(true, 300);  // a few events now, fewer than a record's worth
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 20000));
  w.run(5000);
  CHECK(journalWrites(b, ops1).empty());  // waiting: not 14 yet, nor 10 min
  b.powerOff();
  w.run(1000);
  b.powerOn();
  CHECK(w.runUntil([&] { return b.running(); }, 2000));
  w.run(5000);
  std::vector<Ev> j = journal(b);
  CHECK_EQ(countEv(j, "boot", (int)boot0), 1);
  CHECK_EQ(countEv(j, "gate_state", (int)boot0), 0);  // the move's, never written
}

// [event-log] [relay-pulse] A batch falling due while a relay pulses waits for it to end: a flash write stops the loop
// ~0.5 s, and Relay::update couldn't end the pulse on time.
TEST(event_log_never_writes_during_a_pulse) {
  Site w;
  w.commission([](Board &b) { CHECK(b.set("pulse_ms", 3000)); });
  w.run(WAIT_MS + 5000);  // everything so far written
  for (int i = 0; i < 6; i++) {  // moves until a batch falls due inside a pulse
    w.user(i % 2 == 0);
    w.run(25000);
  }
  // Every journal write on the gate starts outside its pulses (from its log): pulse at t lasts pulse_ms.
  for (const Board::FlashOp &op : journalWrites(w.gate)) {
    for (const LogEv &e : w.gate.logs) {
      if (e.ev != "pulse") continue;
      int32_t d = (int32_t)(op.at - e.at);
      if (d >= 0 && d < 3000) throw Failure("a journal write " + std::to_string(d) + " ms into a pulse");
    }
  }
}

// [event-log] A flood of replayed frames: each one is still an event on the console, but the ring coalesces them into
// one entry with its count (so they can't push everything else out), and the journal is written at most once a
// minute however fast they come.
TEST(event_log_flood_coalesced_and_written_at_most_once_a_minute) {
  Site w;
  w.commission();
  Bytes kept;
  w.drop = [&](const AirFrame &f) {
    if (f.from == w.gate.idx && f.type() == 5 && kept.empty()) kept = f.b;
    return false;
  };
  CHECK(w.runUntil([&] { return !kept.empty(); }, 40000));
  w.drop = nullptr;
  w.run(60000);  // the link has moved well past it
  w.run(WAIT_MS);
  size_t ops = w.house.flashOps.size();
  int replays = w.house.count("replay");
  const int FLOOD_S = 300;
  for (int i = 0; i < FLOOD_S * 10; i++) {
    w.inject(w.house, kept);
    w.run(100);
  }
  int seen = w.house.count("replay") - replays;
  CHECK(seen >= FLOOD_S * 10 - 50);  // every replay is still an event
  size_t writes = journalWrites(w.house, ops).size();
  CHECK(writes <= (size_t)(FLOOD_S * 1000 / GAP_MS + 1));
  // An entry counts up to 256: a dozen entries for thousands of replays, all of them counted.
  int inRing = 0;
  for (const Ev &e : ring(w.house))
    if (e.ev == "replay") inRing += e.n;
  CHECK_EQ(inRing, seen);
  CHECK(countEv(ring(w.house), "replay") <= seen / 256 + 1);
  w.run(WAIT_MS + GAP_MS);  // the rest written
  int counted = 0;
  for (const Ev &e : journal(w.house))
    if (e.ev == "replay") counted += e.n;
  CHECK_EQ(counted, seen);
  CHECK(w.houseLinkUp());
}

// [event-log] log.get before: past the oldest record, an empty page with no cursor; 0 or not a uint32, refused.
TEST(event_log_get_before_arguments) {
  Site w;
  w.commission();
  JsonDocument r = w.gate.request("log.get", "\"before\":1");
  CHECK(r["ok"] == true);
  CHECK(r["log"].as<JsonArrayConst>().size() == 0);
  CHECK(r["seq"].isNull());
  for (const char *bad : { "0", "-1", "4294967296", "\"9\"" }) {
    JsonDocument e = w.gate.request("log.get", std::string("\"before\":") + bad);
    CHECK(e["ok"] == false);
    CHECK(e["error"] == "before must be 1..4294967295");
  }
  JsonDocument all = w.gate.request("log.get");
  CHECK(all["ok"] == true);
  CHECK(all["boot"].as<uint32_t>() == bootCount(w.gate));
}

// [event-log] A chattering input coalesces into one entry that keeps its first time and its latest level: the gate's
// AC sense dropping and coming back reads as back, not as still off.
TEST(event_log_coalesced_input_keeps_its_latest_level) {
  Site w;
  w.commission();
  w.run(WAIT_MS + 4 * GAP_MS);  // nothing waiting
  int inputs = w.gate.count("input");
  for (int i = 0; i < 6; i++) {  // AC off and on, three times
    w.opener.force[2] = i % 2 ? 1 : 0;
    w.run(600);
  }
  w.opener.force[2] = -1;
  w.run(600);
  CHECK_EQ(w.gate.count("input") - inputs, 6);  // every edge still an event on the console
  std::vector<Ev> in;
  for (const Ev &e : ring(w.gate))
    if (e.ev == "input" && e.a == 3) in.push_back(e);
  CHECK_EQ(in.size(), 1u);
  CHECK(in[0].n >= 6);
  CHECK_EQ(in[0].b, 1);  // its latest level: AC back
}
