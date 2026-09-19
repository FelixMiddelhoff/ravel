// The same sweep against the fixed server.
#include <cstdio>

#include "deposit.hpp"
#include "ravel/runner.hpp"

// [deposit_fixed_main]
int main() {
  ravel::RunnerOptions options;
  options.seed_count = 1000;

  const ravel::RunnerReport report = ravel::run_seeds(deposit_setup(/*dedupe=*/true), options);
  std::printf("%llu seeds run, %zu failed\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  return report.ok() ? 0 : 1;
}
// [/deposit_fixed_main]
