// Raft under fire. A three-node cluster elects leaders and replicates a log
// while nodes lose power at random, the network drops, delays and reorders
// messages, and each node's disk loses whatever it had not synced.
//
// The correct implementation survives every seed. Three deliberate bugs, of the
// kind real Raft implementations have shipped with, are each found; the first is
// then shrunk to a much smaller run and replayed to show what went wrong.
//
//   ravel_raft [seeds]
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "raft.hpp"
#include "ravel/runner.hpp"

namespace {

void report(raft::Bug bug, std::uint64_t seeds, bool shrink) {
  raft::Config config;
  config.bug = bug;

  ravel::RunnerOptions options;
  options.seed_count = seeds;
  options.shrink_first_failure = shrink;
  options.max_shrink_attempts = 1000;
  options.simulation = raft::options(config);
  options.simulation.trace_dir = "ravel-traces";

  const ravel::RunnerReport run = ravel::run_seeds(raft::setup(config), options);
  std::printf("%-28s %4zu of %llu seeds fail\n", raft::to_string(bug), run.failures.size(),
              static_cast<unsigned long long>(run.seeds_run));
  if (run.ok()) return;

  const ravel::Result& first = run.failures.front();
  std::printf("  first failing seed %llu: %s\n", static_cast<unsigned long long>(first.seed),
              first.failure.c_str());
  if (!run.shrunk) return;

  const ravel::ShrinkResult& shrunk = *run.shrunk;
  std::printf("  shrunk from %zu random choices (%llu steps) to %zu (%llu steps), %llu replays\n",
              shrunk.original_choices.size(), static_cast<unsigned long long>(shrunk.original.steps),
              shrunk.choices.size(), static_cast<unsigned long long>(shrunk.minimal.steps),
              static_cast<unsigned long long>(shrunk.attempts));
  std::printf("  reproducer: %s\n  trace:      %s\n", shrunk.choices_path.c_str(),
              shrunk.minimal.trace_path.c_str());

  // Replay the minimal run and ask the checkers what they saw.
  auto history = std::make_shared<raft::History>();
  ravel::SimulationOptions replay = raft::options(config);
  replay.replay_choices = shrunk.choices;
  ravel::Simulation sim(first.seed, replay);
  raft::setup(config, history)(sim);
  sim.run_until_quiescent();
  for (const auto* violations : {&history->election_violations, &history->state_machine_violations,
                                 &history->completeness_violations}) {
    for (const std::string& violation : *violations) std::printf("  violation: %s\n", violation.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t seeds = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 600;

  report(raft::Bug::None, seeds, /*shrink=*/false);
  report(raft::Bug::SkipFileSync, seeds, /*shrink=*/true);
  report(raft::Bug::SkipDirectorySync, seeds, /*shrink=*/false);
  report(raft::Bug::IgnoreLogUpToDateCheck, seeds, /*shrink=*/false);
  return 0;
}
