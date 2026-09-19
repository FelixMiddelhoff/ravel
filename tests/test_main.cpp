#include <cstdio>

#include "testing.hpp"

int main() {
  int failed_tests = 0;
  for (const auto& test : ravel::testing::registry()) {
    const int failures_before = ravel::testing::failure_count();
    test.run();
    const bool passed = ravel::testing::failure_count() == failures_before;
    std::printf("%s %s\n", passed ? "[ ok ]" : "[FAIL]", test.name);
    if (!passed) ++failed_tests;
  }
  std::printf("%zu tests, %d failed\n", ravel::testing::registry().size(), failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
