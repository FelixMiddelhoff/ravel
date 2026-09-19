#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "ravel/shrink.hpp"
#include "ravel/simulation.hpp"

namespace ravel {

/// How run_seeds runs.
struct RunnerOptions {
  /// The first seed to run.
  std::uint64_t first_seed = 0;
  /// How many seeds to run, starting at first_seed.
  std::uint64_t seed_count = 1000;

  /// Worker threads; 0 means one per hardware thread. The report does not
  /// depend on this number.
  unsigned threads = 0;

  /// Report only the lowest failing seed and skip seeds beyond it.
  bool stop_at_first_failure = false;

  /// Shrink the lowest failing seed once the sweep is done (see shrink()).
  bool shrink_first_failure = false;
  /// Replay budget for shrinking; see ShrinkOptions::max_attempts.
  std::uint64_t max_shrink_attempts = ShrinkOptions{}.max_attempts;

  SimulationOptions simulation;  ///< Applied to every run.
};

/// What run_seeds found.
struct RunnerReport {
  /// Seeds covered. With stop_at_first_failure, the seeds up to and including
  /// the first failing one.
  std::uint64_t seeds_run = 0;

  std::vector<Result> failures;  ///< In ascending seed order.

  /// The lowest failing seed, minimized. Set only if shrink_first_failure was
  /// requested and something failed.
  std::optional<ShrinkResult> shrunk;

  /// True if no seed failed.
  bool ok() const noexcept { return failures.empty(); }
};

/// Runs the seeds first_seed ... first_seed + seed_count - 1, in parallel,
/// each in its own Simulation. The report is a function of the setup and the
/// seed range alone: the same on every machine and for every thread count.
///
/// A setup that throws fails its seed rather than the whole run. Setup runs
/// on several threads at once; see SimulationSetup.
RunnerReport run_seeds(const SimulationSetup& setup, const RunnerOptions& options = {});

}  // namespace ravel
