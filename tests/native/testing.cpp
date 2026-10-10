// Test runner: runs the registered tests (all, or those named on the command line) and exits 1 on any failure.
#include "testing.h"
#include <stdio.h>
#include <string.h>
#include <chrono>

std::vector<TestCase> &registry() {
  static std::vector<TestCase> r;
  return r;
}

struct HookPair {
  void (*before)();
  void (*after)();
};
static std::vector<HookPair> &hooks() {
  static std::vector<HookPair> h;
  return h;
}
TestHooks::TestHooks(void (*before)(), void (*after)()) {
  hooks().push_back({ before, after });
}

std::string where(const char *file, int line, const std::string &what) {
  const char *base = strrchr(file, '/');
  return std::string(base ? base + 1 : file) + ":" + std::to_string(line) + ": " + what;
}

// Arguments: name substrings (a test runs if its name contains any of them; none = every test), --exact (the names
// must match whole), --shard=I/N (every Nth test from the Ith, so N processes can share the run).
int main(int argc, char **argv) {
  std::vector<const char *> names;
  bool exact = false;
  unsigned shard = 0, shards = 1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--exact")) exact = true;
    else if (sscanf(argv[i], "--shard=%u/%u", &shard, &shards) == 2 && shards && shard < shards) continue;
    else if (!strncmp(argv[i], "--", 2)) {
      fprintf(stderr, "unknown option %s (name substrings, --exact, --shard=I/N)\n", argv[i]);
      return 2;
    } else names.push_back(argv[i]);
  }
  auto wanted = [&](const char *name) {
    if (names.empty()) return true;
    for (const char *n : names)
      if (exact ? !strcmp(name, n) : strstr(name, n) != nullptr) return true;
    return false;
  };
  int failed = 0, run = 0;
  unsigned index = 0;
  for (const TestCase &t : registry()) {
    if (!wanted(t.name) || index++ % shards != shard) continue;
    run++;
    auto started = std::chrono::steady_clock::now();
    std::string err;
    try {
      for (const HookPair &h : hooks())
        if (h.before) h.before();
      t.fn();
    } catch (const std::exception &e) {
      err = e.what();
    }
    // After hooks may fail the test too (e.g. invariant monitors that saw a breach); all of them run.
    for (const HookPair &h : hooks()) {
      if (!h.after) continue;
      try {
        h.after();
      } catch (const std::exception &e) {
        if (err.empty()) err = e.what();
        else err += std::string("\n      ") + e.what();
      }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    char took[32] = "";
    if (secs >= 1) snprintf(took, sizeof(took), " (%.0f s)", secs);  // where a slow run spends its time
    if (t.xfail) {
      if (err.empty()) {
        printf("XPASS %s: fixed? remove XFAIL (%s)\n", t.name, t.xfail);
        failed++;
      } else {
        printf("xfail %s%s (%s)\n", t.name, took, t.xfail);
      }
    } else if (err.empty()) {
      printf("ok    %s%s\n", t.name, took);
    } else {
      printf("FAIL  %s%s\n      %s\n", t.name, took, err.c_str());
      failed++;
    }
    fflush(stdout);
  }
  printf("%d tests, %d failed\n", run, failed);
  return failed ? 1 : 0;
}
