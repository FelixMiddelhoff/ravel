#include <cstdint>
#include <stdexcept>
#include <string>

#include "ravel/ravel.h"
#include "ravel/simulation.hpp"
#include "ravel/version.hpp"
#include "testing.hpp"

namespace {

// A small racy system: three tasks each bump a shared counter across a yield.
// Nothing here is time- or thread-dependent, so the run is fully determined
// by the seed.
void setup_counter_system(ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);
  for (int i = 0; i < 3; ++i) {
    sim.scheduler().spawn("incrementer", [&sim, &counter]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;  // Lost update if another task ran in between.
    });
  }
  sim.add_invariant("no_lost_updates", [&counter] { return counter == 3; });
}

ravel::Result run_counter_system(std::uint64_t seed) {
  ravel::Simulation sim(seed);
  setup_counter_system(sim);
  return sim.run_until_quiescent();
}

}  // namespace

TEST(simulation_passes_when_invariants_hold) {
  ravel::Simulation sim(1);
  int counter = 0;
  sim.scheduler().spawn("incr", [&]() -> ravel::Task {
    ++counter;
    co_return;
  });
  sim.add_invariant("counter_incremented", [&] { return counter == 1; });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(result.ok);
  CHECK(result.seed == 1);
  CHECK(result.failure.empty());
}

TEST(simulation_names_the_failed_invariant) {
  ravel::Simulation sim(1);
  sim.add_invariant("always_false", [] { return false; });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.failure == "invariant 'always_false' failed");
}

TEST(simulation_treats_a_throwing_invariant_as_failed) {
  ravel::Simulation sim(1);
  sim.add_invariant("explodes", []() -> bool { throw std::runtime_error("boom"); });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.failure == "invariant 'explodes' threw");
}

TEST(simulation_fails_the_run_when_a_task_throws) {
  ravel::Simulation sim(1);
  sim.scheduler().spawn("bad", []() -> ravel::Task {
    throw std::runtime_error("boom");
    co_return;
  });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.failure == "task 'bad' threw: boom");
}

TEST(simulation_fails_the_run_at_the_step_limit) {
  ravel::Simulation sim(1, ravel::SimulationOptions{.max_steps = 50});
  sim.scheduler().spawn("spinner", [&]() -> ravel::Task {
    while (true) co_await sim.scheduler().yield();
  });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.steps == 50);
}

// The property the whole library sells: a seed replays exactly.
TEST(simulation_replays_a_seed_exactly) {
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    const ravel::Result first = run_counter_system(seed);
    const ravel::Result second = run_counter_system(seed);
    CHECK(first.ok == second.ok);
    CHECK(first.failure == second.failure);
    CHECK(first.steps == second.steps);
    CHECK(first.trace_digest == second.trace_digest);
  }
}

// ...and the reason to run many seeds: some interleavings expose the bug and
// others do not.
TEST(simulation_finds_the_lost_update_bug_on_some_seeds_but_not_all) {
  int failing_seeds = 0;
  for (std::uint64_t seed = 0; seed < 100; ++seed) {
    if (!run_counter_system(seed).ok) ++failing_seeds;
  }
  CHECK(failing_seeds > 0);
  CHECK(failing_seeds < 100);
}

TEST(simulation_c_api_runs_a_simulation) {
  ravel_simulation* sim = ravel_simulation_create(1);
  CHECK(sim != nullptr);
  CHECK(ravel_simulation_run(sim) == 1);
  ravel_simulation_destroy(sim);

  CHECK(ravel_simulation_run(nullptr) == 0);
  ravel_simulation_destroy(nullptr);  // Like free(NULL): harmless.
}

TEST(version_string_matches_version_constants) {
  const std::string expected = std::to_string(ravel::kVersionMajor) + "." +
                               std::to_string(ravel::kVersionMinor) + "." +
                               std::to_string(ravel::kVersionPatch);
  CHECK(expected == ravel::version_string());
  CHECK(expected == ravel_version_string());
}

// Pins one whole run (scheduling decisions included) to a fixed digest. Every
// platform in CI must produce it; if it changes, saved seeds no longer replay
// and the change is seed-breaking (see the seed stability policy).
TEST(simulation_trace_digest_matches_golden_value) {
  const ravel::Result result = run_counter_system(1);
  CHECK(result.trace_digest == 0x6DBCA5AC75180547ULL);
}

// A run's recorded choices replay it exactly, with no help from the seed.
TEST(simulation_replays_from_its_recorded_choices) {
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    ravel::Simulation original(seed);
    setup_counter_system(original);
    const ravel::Result first = original.run_until_quiescent();

    ravel::Simulation replay(/*seed=*/999,
                             ravel::SimulationOptions{.replay_choices = original.choices()});
    setup_counter_system(replay);
    const ravel::Result second = replay.run_until_quiescent();

    CHECK(first.ok == second.ok);
    CHECK(first.trace_digest == second.trace_digest);
    CHECK(replay.choices() == original.choices());
  }
}

TEST(simulation_reports_a_task_that_throws_something_unnamed) {
  ravel::Simulation sim(1);
  sim.scheduler().spawn("odd", []() -> ravel::Task {
    throw 42;
    co_return;
  });

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.failure == "task 'odd' threw: unknown exception");
}
