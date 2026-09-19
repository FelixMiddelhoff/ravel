#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ravel/simulation.hpp"

namespace ravel {

struct DeterminismOptions {
  std::uint64_t first_seed = 0;
  std::uint64_t seed_count = 100;

  // Worker threads; 0 means one per hardware thread. As with run_seeds, the
  // setup then runs on several threads at once.
  unsigned threads = 0;

  SimulationOptions simulation;  // Applied to every run.
};

struct DeterminismProblem {
  std::uint64_t seed = 0;
  std::string description;  // What differed, and where.
};

struct DeterminismReport {
  std::uint64_t seeds_checked = 0;
  std::vector<DeterminismProblem> problems;  // In ascending seed order.

  bool ok() const noexcept { return problems.empty(); }
};

// Checks that the code under test really is deterministic, which everything
// else (replay, shrinking, saved reproducers) depends on. For each seed it
//   1. runs the setup twice and compares the runs event by event, and
//   2. replays the first run from its recorded random choices and compares
//      that too.
// Any difference means something outside ravel's control leaks into the run:
// a real clock or random source, a thread, a static or global that survives
// from one run to the next, iteration over a container ordered by pointer
// value, uninitialized memory.
//
// Each problem names the first step where the runs part ways, which is
// usually enough to find the culprit. A few dozen seeds is normally plenty; a
// leak that only shows up on rare paths needs the seeds that reach them.
//
// Setups run several times per seed, so they must not have side effects that
// carry over, except the very leaks this is looking for.
DeterminismReport check_determinism(const SimulationSetup& setup,
                                    const DeterminismOptions& options = {});

}  // namespace ravel
