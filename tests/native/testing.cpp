// Test runner: runs the registered tests (all, or those named on the command line) and exits 1 on any failure.
#include "testing.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include <algorithm>
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

// The next test's position in a queue file shared by the processes running the tests: the count taken so far, under
// an exclusive lock (an empty file is 0). A queue that fails ends the run loudly rather than losing tests.
static size_t takeFromQueue(int fd) {
  unsigned n = 0;
  bool ok = flock(fd, LOCK_EX) == 0 && pread(fd, &n, sizeof(n), 0) >= 0;
  unsigned next = n + 1;
  ok = ok && pwrite(fd, &next, sizeof(next), 0) == (ssize_t)sizeof(next);
  flock(fd, LOCK_UN);
  if (!ok) {
    perror("test queue");
    exit(2);
  }
  return n;
}

// Arguments: name substrings (a test runs if its name contains any of them; none = every test), --exact (the names
// must match whole), --queue=FILE (processes given the same empty FILE share the run: each takes the next test from
// it, slow ones first, until none are left).
int main(int argc, char **argv) {
  std::vector<const char *> names;
  bool exact = false;
  int queue = -1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--exact")) exact = true;
    else if (!strncmp(argv[i], "--queue=", 8)) {
      queue = open(argv[i] + 8, O_RDWR | O_CREAT, 0644);
      if (queue < 0) {
        perror(argv[i] + 8);
        return 2;
      }
    } else if (!strncmp(argv[i], "--", 2)) {
      fprintf(stderr, "unknown option %s (name substrings, --exact, --queue=FILE)\n", argv[i]);
      return 2;
    } else names.push_back(argv[i]);
  }
  auto wanted = [&](const char *name) {
    if (names.empty()) return true;
    for (const char *n : names)
      if (exact ? !strcmp(name, n) : strstr(name, n) != nullptr) return true;
    return false;
  };
  std::vector<const TestCase *> todo;
  for (const TestCase &t : registry())
    if (wanted(t.name)) todo.push_back(&t);
  std::stable_partition(todo.begin(), todo.end(), [](const TestCase *t) { return t->slow; });
  int failed = 0, run = 0;
  for (size_t i = 0;; i++) {
    size_t next = queue >= 0 ? takeFromQueue(queue) : i;
    if (next >= todo.size()) break;
    const TestCase &t = *todo[next];
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
      if (!t.slow && secs >= 15) printf("      over 15 s: if it always is, mark it SLOW_TEST to start it first\n");
    } else {
      printf("FAIL  %s%s\n      %s\n", t.name, took, err.c_str());
      failed++;
    }
    fflush(stdout);
  }
  printf("%d tests, %d failed\n", run, failed);
  return failed ? 1 : 0;
}
