#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "ravel/simulation.hpp"

namespace ravel {

// Builds one run: spawn tasks, add channels and invariants. Called once per
// seed on a fresh Simulation, possibly from several threads at once, so it
// must not touch shared mutable state. Keep per-run state in
// Simulation::make_state instead of in captured variables.
using SimulationSetup = std::function<void(Simulation&)>;

struct RunnerOptions {
  std::uint64_t first_seed = 0;
  std::uint64_t seed_count = 1000;

  // Worker threads; 0 means one per hardware thread. The report does not
  // depend on this number.
  unsigned threads = 0;

  // Report only the lowest failing seed and skip seeds beyond it.
  bool stop_at_first_failure = false;

  SimulationOptions simulation;  // Applied to every run.
};

struct RunnerReport {
  // Seeds covered. With stop_at_first_failure, the seeds up to and including
  // the first failing one.
  std::uint64_t seeds_run = 0;

  std::vector<Result> failures;  // In ascending seed order.

  bool ok() const noexcept { return failures.empty(); }
};

// Runs the seeds first_seed ... first_seed + seed_count - 1, in parallel,
// each in its own Simulation. The report is a function of the setup and the
// seed range alone: the same on every machine and for every thread count.
//
// A setup that throws fails its seed rather than the whole run.
RunnerReport run_seeds(const SimulationSetup& setup, const RunnerOptions& options = {});

}  // namespace ravel
