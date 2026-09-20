#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ravel/runner.hpp"
#include "testing.hpp"

namespace {

// Three tasks read the counter, yield, then write back read+1. Some
// interleavings lose an update; the same system as in the simulation tests.
void setup_lost_update_system(ravel::Simulation& sim) {
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

ravel::RunnerOptions options_for(unsigned threads) {
  ravel::RunnerOptions options;
  options.seed_count = 200;
  options.threads = threads;
  return options;
}

std::vector<std::uint64_t> failing_seeds(const ravel::RunnerReport& report) {
  std::vector<std::uint64_t> seeds;
  for (const ravel::Result& failure : report.failures) seeds.push_back(failure.seed);
  return seeds;
}

}  // namespace

TEST(runner_reports_a_system_with_no_bug_as_ok) {
  const ravel::RunnerReport report =
      ravel::run_seeds([](ravel::Simulation&) {}, options_for(2));
  CHECK(report.ok());
  CHECK(report.seeds_run == 200);
}

TEST(runner_finds_failing_seeds_and_lists_them_in_order) {
  const ravel::RunnerReport report = ravel::run_seeds(setup_lost_update_system, options_for(1));
  CHECK(!report.ok());
  CHECK(report.failures.size() < 200);
  const std::vector<std::uint64_t> seeds = failing_seeds(report);
  for (std::size_t i = 1; i < seeds.size(); ++i) CHECK(seeds[i - 1] < seeds[i]);
}

TEST(runner_report_does_not_depend_on_thread_count) {
  const ravel::RunnerReport one = ravel::run_seeds(setup_lost_update_system, options_for(1));
  const ravel::RunnerReport four = ravel::run_seeds(setup_lost_update_system, options_for(4));
  CHECK(failing_seeds(one) == failing_seeds(four));
  CHECK(one.failures.size() == four.failures.size());
  for (std::size_t i = 0; i < one.failures.size() && i < four.failures.size(); ++i) {
    CHECK(one.failures[i].trace_digest == four.failures[i].trace_digest);
  }
}

TEST(runner_agrees_with_running_each_seed_by_hand) {
  const ravel::RunnerReport report = ravel::run_seeds(setup_lost_update_system, options_for(3));
  std::vector<std::uint64_t> expected;
  for (std::uint64_t seed = 0; seed < 200; ++seed) {
    ravel::Simulation sim(seed);
    setup_lost_update_system(sim);
    if (!sim.run_until_quiescent().ok) expected.push_back(seed);
  }
  CHECK(failing_seeds(report) == expected);
}

TEST(runner_can_stop_at_the_first_failure) {
  const ravel::RunnerReport all = ravel::run_seeds(setup_lost_update_system, options_for(1));
  CHECK(!all.ok());

  for (const unsigned threads : {1u, 4u}) {
    ravel::RunnerOptions options = options_for(threads);
    options.stop_at_first_failure = true;
    const ravel::RunnerReport report = ravel::run_seeds(setup_lost_update_system, options);

    CHECK(report.failures.size() == 1);
    if (report.failures.empty()) continue;
    CHECK(report.failures.front().seed == all.failures.front().seed);
    CHECK(report.seeds_run == all.failures.front().seed + 1);
  }
}

TEST(runner_counts_seeds_from_the_first_seed_when_it_stops_early) {
  const ravel::RunnerReport all = ravel::run_seeds(setup_lost_update_system, options_for(1));
  CHECK(!all.failures.empty());
  if (all.failures.empty()) return;
  ravel::RunnerOptions options = options_for(1);
  options.stop_at_first_failure = true;
  options.first_seed = all.failures.front().seed;  // Fails at once.
  const ravel::RunnerReport report = ravel::run_seeds(setup_lost_update_system, options);
  CHECK(report.seeds_run == 1);
  options.first_seed = all.failures.front().seed - (all.failures.front().seed > 0 ? 1 : 0);
  CHECK(ravel::run_seeds(setup_lost_update_system, options).seeds_run ==
        (all.failures.front().seed > 0 ? 2 : 1));
}

TEST(runner_honours_the_seed_range) {
  ravel::RunnerOptions options;
  options.first_seed = 1000;
  options.seed_count = 50;
  options.threads = 2;
  const ravel::RunnerReport report = ravel::run_seeds(setup_lost_update_system, options);
  for (const ravel::Result& failure : report.failures) {
    CHECK(failure.seed >= 1000 && failure.seed < 1050);
  }
}

TEST(runner_with_zero_seeds_runs_nothing) {
  ravel::RunnerOptions options;
  options.seed_count = 0;
  const ravel::RunnerReport report = ravel::run_seeds(setup_lost_update_system, options);
  CHECK(report.ok());
  CHECK(report.seeds_run == 0);
}

TEST(runner_fails_a_seed_whose_setup_throws) {
  const ravel::RunnerReport report = ravel::run_seeds(
      [](ravel::Simulation&) { throw std::runtime_error("bad setup"); }, options_for(2));
  CHECK(report.failures.size() == 200);
  CHECK(report.failures.front().failure == "simulation threw: bad setup");
}

TEST(simulation_state_outlives_its_tasks) {
  struct Flag {
    bool* destroyed;
    ~Flag() { *destroyed = true; }
  };
  bool destroyed = false;
  {
    ravel::Simulation sim(1);
    sim.make_state<Flag>(&destroyed);
    CHECK(!destroyed);
  }
  CHECK(destroyed);
}

TEST(runner_fails_a_seed_whose_setup_throws_something_unnamed) {
  const ravel::RunnerReport report =
      ravel::run_seeds([](ravel::Simulation&) { throw 42; }, options_for(2));
  CHECK(report.failures.size() == 200);
  CHECK(report.failures.front().failure == "simulation threw an unknown exception");
}
