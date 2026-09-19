// [first_test]
#include <cstdio>

#include "ravel/runner.hpp"

int main() {
  ravel::RunnerOptions options;
  options.seed_count = 100;

  const ravel::RunnerReport report = ravel::run_seeds(
      [](ravel::Simulation& sim) {
        // One task that sleeps for ten virtual ticks, and a check that always passes.
        sim.scheduler().spawn("hello", [&sim]() -> ravel::Task {
          co_await sim.scheduler().sleep(10);
        });
        sim.add_invariant("always_true", [] { return true; });
      },
      options);

  std::printf("%llu seeds run, %zu failed\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  return report.ok() ? 0 : 1;
}
// [/first_test]
