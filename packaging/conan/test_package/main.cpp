#include <cstdio>
#include <cstdlib>

#include "ravel/runner.hpp"
#include "ravel/version.hpp"

int main() {
  ravel::RunnerOptions options;
  options.seed_count = 10;
  const ravel::RunnerReport report = ravel::run_seeds([](ravel::Simulation&) {}, options);
  std::printf("ravel %s: %s\n", ravel::version_string(), report.ok() ? "ok" : "failed");
  return report.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
