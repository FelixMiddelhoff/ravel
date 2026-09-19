#include <cstdio>
#include <cstring>

#include "testing.hpp"

// Usage: ravel_tests [substring]. With an argument, only tests whose name
// contains it run.
int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int failed_tests = 0;
  std::size_t ran = 0;
  for (const auto& test : ravel::testing::registry()) {
    if (filter != nullptr && std::strstr(test.name, filter) == nullptr) continue;
    const int failures_before = ravel::testing::failure_count();
    test.run();
    ++ran;
    const bool passed = ravel::testing::failure_count() == failures_before;
    std::printf("%s %s\n", passed ? "[ ok ]" : "[FAIL]", test.name);
    std::fflush(stdout);  // A crash in the next test must not lose these lines.
    if (!passed) ++failed_tests;
  }
  std::printf("%zu tests, %d failed\n", ran, failed_tests);
  return failed_tests == 0 ? 0 : 1;
}
