#pragma once

// A deliberately tiny test harness: no dependencies, and checks that stay
// active in Release builds (unlike assert()).

#include <cstdio>
#include <vector>

namespace ravel::testing {

struct TestCase {
  const char* name;
  void (*run)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char* name, void (*run)()) { registry().push_back({name, run}); }
};

inline void report_failed_check(const char* file, int line, const char* expression) {
  std::printf("    %s:%d: CHECK(%s) failed\n", file, line, expression);
  ++failure_count();
}

}  // namespace ravel::testing

// Defines and registers a test case:  TEST(name) { CHECK(...); }
#define TEST(name)                                                        \
  static void name();                                                     \
  static const ravel::testing::Registrar name##_registrar(#name, &name); \
  static void name()

// Records a failure and keeps going, so one run reports every broken check.
#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) ravel::testing::report_failed_check(__FILE__, __LINE__, #condition); \
  } while (false)
