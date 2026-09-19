// A racy counter: three tasks read the counter, yield, then write back
// read+1. If another task runs in between, an update is lost. Most seeds
// happen to survive; some do not. The runner finds a failing seed, and that
// seed alone replays the failure exactly, and shrinking boils it down to the
// smallest run that still fails.
#include <cstdio>

#include "ravel/runner.hpp"

namespace {

void setup(ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);

  for (int i = 0; i < 3; ++i) {
    sim.scheduler().spawn("incrementer", [&sim, &counter]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;
    });
  }
  sim.add_invariant("no_lost_updates", [&counter] { return counter == 3; });
}

}  // namespace

int main() {
  ravel::RunnerOptions options;
  options.seed_count = 100;
  options.stop_at_first_failure = true;
  options.shrink_first_failure = true;
  options.simulation.trace_dir = "ravel-traces";

  const ravel::RunnerReport report = ravel::run_seeds(setup, options);
  if (report.ok()) {
    std::puts("no failing seed found");
    return 0;
  }

  const ravel::Result& failure = report.failures.front();
  std::printf("seed %llu failed: %s\ntrace: %s\n", static_cast<unsigned long long>(failure.seed),
              failure.failure.c_str(), failure.trace_path.c_str());

  const ravel::ShrinkResult& shrunk = *report.shrunk;
  std::printf("shrunk from %zu random choices to %zu (%s)\n", shrunk.original_choices.size(),
              shrunk.choices.size(),
              shrunk.choices.empty() ? "it fails under plain round-robin" : "see the choices file");
  std::printf("minimal choices: %s\n", shrunk.choices_path.c_str());

  // Replaying the seed by hand reproduces the identical run.
  ravel::Simulation replay(failure.seed);
  setup(replay);
  const bool identical = replay.run_until_quiescent().trace_digest == failure.trace_digest;
  std::printf("replay of seed %llu %s\n", static_cast<unsigned long long>(failure.seed),
              identical ? "is identical" : "DIVERGED");
  return 1;
}
