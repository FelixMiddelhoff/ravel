#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ravel/clock.hpp"
#include "ravel/network.hpp"
#include "ravel/rng.hpp"
#include "ravel/scheduler.hpp"
#include "ravel/trace.hpp"

namespace ravel {

// Returns true while the property holds.
using InvariantFn = std::function<bool()>;

struct SimulationOptions {
  // Upper bound on scheduler steps, so a livelocked system fails the run
  // instead of hanging it.
  std::uint64_t max_steps = 1'000'000;
};

struct Result {
  bool ok = false;
  std::uint64_t seed = 0;
  std::string failure;             // What went wrong; empty when ok.
  std::uint64_t steps = 0;         // Scheduler steps taken.
  std::uint64_t trace_digest = 0;  // Equal digests mean identical runs.
};

// Top-level harness: owns the seed, virtual clock, RNG, trace and scheduler
// for one deterministic run. Running the same code under the same seed gives
// the same Result, including the same trace_digest.
class Simulation {
 public:
  explicit Simulation(std::uint64_t seed, SimulationOptions options = {});

  // The scheduler refers to the members below, so a Simulation cannot move.
  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  VirtualClock& clock() noexcept { return clock_; }
  VirtualRng& rng() noexcept { return rng_; }
  Scheduler& scheduler() noexcept { return scheduler_; }
  const Trace& trace() const noexcept { return trace_; }

  Channel& add_channel(std::string from, std::string to, FaultSpec fault);

  // Invariants are checked once, after the scheduler has run to quiescence.
  // An invariant that throws counts as failed.
  void add_invariant(std::string name, InvariantFn invariant);

  Result run_until_quiescent();

 private:
  struct NamedInvariant {
    std::string name;
    InvariantFn check;
  };

  // Names the first invariant that fails, or returns an empty string.
  std::string first_failed_invariant() const;

  // Declaration order matters: the scheduler is built from the three above it.
  std::uint64_t seed_;
  SimulationOptions options_;
  VirtualClock clock_;
  VirtualRng rng_;
  Trace trace_;
  Scheduler scheduler_;

  std::deque<Channel> channels_;  // A deque so add_channel's reference stays valid.
  std::vector<NamedInvariant> invariants_;
};

}  // namespace ravel
