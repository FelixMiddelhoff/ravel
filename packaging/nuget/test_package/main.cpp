#include <cstdio>

#include "ravel/runner.hpp"

int main() {
  ravel::RunnerOptions options;
  options.seed_count = 20;
  const ravel::RunnerReport report = ravel::run_seeds(
      [](ravel::Simulation& sim) {
        const std::uint64_t roll = sim.rng().next_below(4);
        sim.add_invariant("roll_is_not_three", [roll] { return roll != 3; });
      },
      options);
  std::printf("seeds run: %llu, failing: %zu\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  // The invariant fails for some of these seeds; the point is that this links and runs.
  return report.seeds_run == 20 && !report.failures.empty() ? 0 : 1;
}
