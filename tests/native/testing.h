// Minimal test framework shared by the host test programs (tests: link and config; systests: both boards' whole
// firmware in the simulated site, see world.h).
#pragma once
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase {
  const char *name;
  void (*fn)();
  const char *xfail;  // known failure (reason); the run fails if it unexpectedly passes
  bool slow;          // runs first, so parallel processes (--queue) don't end the run waiting on it
};
std::vector<TestCase> &registry();
struct Reg {
  Reg(const char *n, void (*f)(), const char *xfail = nullptr, bool slow = false) {
    registry().push_back({ n, f, xfail, slow });
  }
};
// Called before and after every test (each program registers what it needs to reset).
struct TestHooks {
  TestHooks(void (*before)(), void (*after)());
};
struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
std::string where(const char *file, int line, const std::string &what);

#define TEST_REG_(name, ...) \
  static void name(); \
  static Reg reg_##name(#name, name, __VA_ARGS__); \
  static void name()
#define TEST(name) TEST_REG_(name, nullptr)
// A test that takes many times longer than the rest (simulated hours).
#define SLOW_TEST(name) TEST_REG_(name, nullptr, true)
// A test that documents a known bug (TODO.md item): it must fail until the fix lands.
#define XFAIL_TEST(name, reason) TEST_REG_(name, reason)
#define CHECK(c) \
  do { \
    if (!(c)) throw Failure(where(__FILE__, __LINE__, "CHECK(" #c ")")); \
  } while (0)
#define CHECK_EQ(a, b) \
  do { \
    long long a_ = (long long)(a), b_ = (long long)(b); \
    if (a_ != b_) \
      throw Failure(where(__FILE__, __LINE__, \
                          "CHECK_EQ(" #a ", " #b "): " + std::to_string(a_) + " != " + std::to_string(b_))); \
  } while (0)
// Inclusive range check.
#define CHECK_IN(v, lo, hi) \
  do { \
    long long v_ = (long long)(v), lo_ = (long long)(lo), hi_ = (long long)(hi); \
    if (v_ < lo_ || v_ > hi_) \
      throw Failure(where(__FILE__, __LINE__, \
                          "CHECK_IN(" #v "): " + std::to_string(v_) + " not in [" + std::to_string(lo_) + ", " + \
                              std::to_string(hi_) + "]")); \
  } while (0)
