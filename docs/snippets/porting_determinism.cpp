// What check_determinism says about code that leaks state between runs.

#include <cstdio>

#include "ravel/determinism.hpp"

// [leaky]
// A function-level static keeps its value from one run to the next, so the
// same seed does not behave the same way twice.
void leaky_setup(ravel::Simulation& sim) {
  static int runs_so_far = 0;
  const ravel::VirtualClock::Tick nap = (++runs_so_far % 2 == 0) ? 5 : 10;

  sim.scheduler().spawn("worker", [&sim, nap]() -> ravel::Task {
    co_await sim.scheduler().sleep(nap);
  });
}
// [/leaky]

// [check]
int main() {
  ravel::DeterminismOptions options;
  options.seed_count = 3;
  options.threads = 1;  // This setup shares a static, so run it on one thread.

  const ravel::DeterminismReport report = ravel::check_determinism(leaky_setup, options);
  std::printf("%llu seeds checked, %zu with problems\n",
              static_cast<unsigned long long>(report.seeds_checked), report.problems.size());
  for (const ravel::DeterminismProblem& problem : report.problems) {
    std::printf("seed %llu: %s\n", static_cast<unsigned long long>(problem.seed),
                problem.description.c_str());
  }
  return report.ok() ? 1 : 0;  // This example is supposed to find something.
}
// [/check]
