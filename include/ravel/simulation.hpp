#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
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

  // When set, a failed run writes its trace here as
  // `ravel-seed-<seed>.trace.jsonl`. Empty (the default) writes no files.
  std::filesystem::path trace_dir;
};

struct Result {
  bool ok = false;
  std::uint64_t seed = 0;
  std::string failure;             // What went wrong; empty when ok.
  std::uint64_t steps = 0;         // Scheduler steps taken.
  std::uint64_t trace_digest = 0;  // Equal digests mean identical runs.
  std::string trace_path;          // The dumped trace; empty if none was written.
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

  // Creates an object owned by the simulation and returns a reference to it.
  // Use it for state shared by tasks and invariants: it outlives every task,
  // and each run (say, each seed of run_seeds) gets its own fresh copy.
  //
  //   int& counter = sim.make_state<int>(0);
  template <typename T, typename... Args>
  T& make_state(Args&&... args) {
    auto state = std::make_shared<T>(std::forward<Args>(args)...);
    T& reference = *state;
    owned_state_.push_back(std::move(state));
    return reference;
  }

  // Invariants are checked once, after the scheduler has run to quiescence.
  // An invariant that throws counts as failed.
  void add_invariant(std::string name, InvariantFn invariant);

  Result run_until_quiescent();

  // Writes the trace as JSON Lines: a header line (format, versions, seed),
  // then one line per event with its step, virtual time, kind, and the id and
  // name of the task or channel it concerns.
  void write_trace(std::ostream& out) const;

 private:
  struct NamedInvariant {
    std::string name;
    InvariantFn check;
  };

  // Names the first invariant that fails, or returns an empty string.
  std::string first_failed_invariant() const;

  // Dumps the trace into options_.trace_dir. Returns the file's path, or an
  // empty string if it could not be written.
  std::string dump_trace() const;

  // "from->to" for a channel, the task's name for a task.
  std::string subject_name(const TraceEvent& event) const;

  // Declared first so it is destroyed last: suspended tasks may still refer
  // to this state while they are torn down.
  std::vector<std::shared_ptr<void>> owned_state_;

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
