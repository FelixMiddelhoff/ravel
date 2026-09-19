#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ravel/clock.hpp"
#include "ravel/disk.hpp"
#include "ravel/network.hpp"
#include "ravel/rng.hpp"
#include "ravel/scheduler.hpp"
#include "ravel/trace.hpp"

namespace ravel {

class Simulation;

/// Returns true while the property holds.
using InvariantFn = std::function<bool()>;

/// Builds one run: spawn tasks, add channels and invariants. Called on a fresh
/// Simulation each time, so a runner may call it from several threads at once:
/// it must not touch shared mutable state. Keep per-run state in
/// Simulation::make_state instead of in captured variables.
using SimulationSetup = std::function<void(Simulation&)>;

/// Options for one run. Every field has a sensible default.
struct SimulationOptions {
  /// Upper bound on scheduler steps, so a livelocked system fails the run
  /// instead of hanging it.
  std::uint64_t max_steps = 1'000'000;

  /// Virtual time after which the run stops, without failing. Timers due later
  /// never fire. Set it for systems that never go quiet on their own, such as
  /// ones with heartbeats. The default lets a run go on until nothing is left.
  VirtualClock::Tick time_limit = std::numeric_limits<VirtualClock::Tick>::max();

  /// When set, a failed run writes its trace here as
  /// `ravel-seed-<seed>.trace.jsonl`. Empty (the default) writes no files.
  std::filesystem::path trace_dir;

  /// When set, the run answers every random draw from this list instead of the
  /// seed (see VirtualRng::replaying). Used to replay and shrink failures.
  std::optional<std::vector<VirtualRng::Choice>> replay_choices;
};

/// How one run ended.
struct Result {
  /// True if the run passed every check.
  bool ok = false;
  /// The seed the run was made from.
  std::uint64_t seed = 0;
  std::string failure;             ///< What went wrong; empty when ok.
  std::uint64_t steps = 0;         ///< Scheduler steps taken.
  std::uint64_t trace_digest = 0;  ///< Equal digests mean identical runs.
  std::string trace_path;          ///< The dumped trace; empty if none was written.
};

/// Top-level harness: owns the seed, virtual clock, RNG, trace and scheduler
/// for one deterministic run. Running the same code under the same seed gives
/// the same Result, including the same trace_digest.
class Simulation {
 public:
  /// A run for `seed`. Then add channels and disks, spawn tasks, add invariants, and run.
  explicit Simulation(std::uint64_t seed, SimulationOptions options = {});

  /// The scheduler refers to the members below, so a Simulation cannot move.
  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  /// The virtual clock.
  VirtualClock& clock() noexcept { return clock_; }
  /// The run's one source of randomness. Everything random in the code under test must come from here.
  VirtualRng& rng() noexcept { return rng_; }
  /// The scheduler: spawn tasks through it.
  Scheduler& scheduler() noexcept { return scheduler_; }
  /// Everything that has happened so far.
  const Trace& trace() const noexcept { return trace_; }

  /// Every random choice made so far. Passing this list back as
  /// SimulationOptions::replay_choices reproduces the run without the seed.
  const std::vector<VirtualRng::Choice>& choices() const noexcept { return rng_.choices(); }

  /// Adds a one-way message channel from endpoint `from` to endpoint `to`.
  Channel& add_channel(std::string from, std::string to, FaultSpec fault);
  /// Adds a virtual disk.
  Disk& add_disk(std::string name, DiskFaultSpec fault = {});

  /// Creates an object owned by the simulation and returns a reference to it.
  /// Use it for state shared by tasks and invariants: it outlives every task,
  /// and each run (say, each seed of run_seeds) gets its own fresh copy.
  ///
  ///   int& counter = sim.make_state<int>(0);
  template <typename T, typename... Args>
  T& make_state(Args&&... args) {
    auto state = std::make_shared<T>(std::forward<Args>(args)...);
    T& reference = *state;
    owned_state_.push_back(std::move(state));
    return reference;
  }

  /// Invariants are checked once, after the scheduler has run to quiescence.
  /// An invariant that throws counts as failed.
  void add_invariant(std::string name, InvariantFn invariant);

  /// Runs until nothing is left to happen (or a task throws, the step limit is hit, or the time
  /// limit passes), then checks the invariants.
  Result run_until_quiescent();

  /// Writes the trace as JSON Lines: a header line (format, versions, seed),
  /// then one line per event with its step, virtual time, kind, and the id and
  /// name of the task or channel it concerns.
  void write_trace(std::ostream& out) const;

  /// One event in words, for messages: `TaskResumed 'client' at t=40`.
  std::string describe(const TraceEvent& event) const;

 private:
  struct NamedInvariant {
    std::string name;
    InvariantFn check;
  };

  /// Names the first invariant that fails, or returns an empty string.
  std::string first_failed_invariant() const;

  /// Dumps the trace into options_.trace_dir. Returns the file's path, or an
  /// empty string if it could not be written.
  std::string dump_trace() const;

  /// "from->to" for a channel, the task's name for a task.
  std::string subject_name(const TraceEvent& event) const;

  /// Declared first so it is destroyed last: suspended tasks may still refer
  /// to this state while they are torn down.
  std::vector<std::shared_ptr<void>> owned_state_;

  /// Declaration order matters: the scheduler is built from the three above it.
  std::uint64_t seed_;
  SimulationOptions options_;
  VirtualClock clock_;
  VirtualRng rng_;
  Trace trace_;
  Scheduler scheduler_;

  /// Deques, so the references handed out by add_channel/add_disk stay valid.
  std::deque<Channel> channels_;
  std::deque<Disk> disks_;
  std::vector<NamedInvariant> invariants_;
};

}  // namespace ravel
