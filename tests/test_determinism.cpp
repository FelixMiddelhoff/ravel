#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "ravel/determinism.hpp"
#include "testing.hpp"

namespace {

ravel::DeterminismOptions single_threaded(std::uint64_t seeds) {
  ravel::DeterminismOptions options;
  options.seed_count = seeds;
  options.threads = 1;  // The leaky setups below share state on purpose.
  return options;
}

// Three tasks racing on a counter, plus a lossy link: plenty of choices, and
// nothing that depends on anything but the seed.
void deterministic_system(ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);
  auto& link = sim.add_channel("a", "b", {.loss_probability = 0.3, .latency_min = 1, .latency_max = 9});
  for (int i = 0; i < 3; ++i) {
    sim.scheduler().spawn("worker", [&sim, &counter, &link]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;
      link.send("done");
    });
  }
  sim.scheduler().spawn("listener", [&link]() -> ravel::Task {
    while (true) co_await link.receive();
  });
  sim.add_invariant("counter_is_three", [&counter] { return counter == 3; });
}

}  // namespace

TEST(check_determinism_passes_a_deterministic_system) {
  const ravel::DeterminismReport report =
      ravel::check_determinism(deterministic_system, single_threaded(50));
  CHECK(report.ok());
  CHECK(report.seeds_checked == 50);
}

TEST(check_determinism_passes_when_run_on_many_threads) {
  ravel::DeterminismOptions options;
  options.seed_count = 50;
  options.threads = 4;
  CHECK(ravel::check_determinism(deterministic_system, options).ok());
}

TEST(check_determinism_catches_state_that_survives_between_runs) {
  // A static: the second run of a seed sees what the first left behind.
  const auto leaky = [](ravel::Simulation& sim) {
    static int runs_so_far = 0;
    const int nap = (++runs_so_far % 2 == 0) ? 5 : 10;
    sim.scheduler().spawn("sleeper", [&sim, nap]() -> ravel::Task {
      co_await sim.scheduler().sleep(static_cast<ravel::VirtualClock::Tick>(nap));
    });
  };

  const ravel::DeterminismReport report = ravel::check_determinism(leaky, single_threaded(3));
  CHECK(!report.ok());
  CHECK(report.problems.size() == 3);
  if (!report.problems.empty()) {
    const std::string& text = report.problems.front().description;
    CHECK(text.find("two runs of the same seed diverged at step") != std::string::npos);
    CHECK(text.find("run 1") != std::string::npos && text.find("run 2") != std::string::npos);
  }
}

TEST(check_determinism_catches_a_real_clock) {
  // Sleeping for however many wall-clock microseconds have passed since some
  // arbitrary moment can never repeat.
  const auto uses_wall_clock = [](ravel::Simulation& sim) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    sim.scheduler().spawn("sleeper", [&sim, ticks]() -> ravel::Task {
      co_await sim.scheduler().sleep(static_cast<ravel::VirtualClock::Tick>(ticks % 1000) + 1);
    });
  };
  CHECK(!ravel::check_determinism(uses_wall_clock, single_threaded(5)).ok());
}

TEST(check_determinism_catches_an_invariant_that_reads_something_unstable) {
  const auto unstable_invariant = [](ravel::Simulation& sim) {
    static int calls = 0;
    sim.add_invariant("flaky", [] { return ++calls % 2 == 0; });
  };
  const ravel::DeterminismReport report =
      ravel::check_determinism(unstable_invariant, single_threaded(1));
  CHECK(!report.ok());
  if (!report.ok()) {
    CHECK(report.problems[0].description.find("an invariant reads something that differs") !=
          std::string::npos);
  }
}

TEST(check_determinism_reports_a_simulation_that_throws) {
  const auto throws = [](ravel::Simulation&) { throw std::runtime_error("bad setup"); };
  const ravel::DeterminismReport report = ravel::check_determinism(throws, single_threaded(2));
  CHECK(report.problems.size() == 2);
  if (!report.problems.empty()) {
    CHECK(report.problems[0].description == "the simulation threw: bad setup");
  }
}

TEST(check_determinism_lists_problems_in_seed_order_and_honours_the_range) {
  const auto always_leaky = [](ravel::Simulation& sim) {
    static int n = 0;
    ++n;
    sim.scheduler().spawn("t", [&sim]() -> ravel::Task { co_await sim.scheduler().yield(); });
    sim.scheduler().spawn("u", [&sim, k = n]() -> ravel::Task {
      co_await sim.scheduler().sleep(static_cast<ravel::VirtualClock::Tick>(k));
    });
  };
  ravel::DeterminismOptions options = single_threaded(4);
  options.first_seed = 100;
  const ravel::DeterminismReport report = ravel::check_determinism(always_leaky, options);
  CHECK(report.problems.size() == 4);
  for (std::size_t i = 0; i < report.problems.size(); ++i) {
    CHECK(report.problems[i].seed == 100 + i);
  }
}

TEST(check_determinism_reports_a_run_that_ends_early) {
  // One run does everything the other does and then a late task's work too.
  // Either run can be the longer one.
  for (const int longer_run : {1, 2}) {
    const auto run_number = std::make_shared<int>(0);
    const auto leaky = [longer_run, run_number](ravel::Simulation& sim) {
      const int run = ++*run_number;
      sim.scheduler().spawn("t", [&sim]() -> ravel::Task { co_await sim.scheduler().yield(); });
      if (run == longer_run) {
        sim.scheduler().call_after(50, [&sim] {
          sim.scheduler().spawn("late", [&sim]() -> ravel::Task { co_await sim.scheduler().yield(); });
        });
      }
    };
    const ravel::DeterminismReport report = ravel::check_determinism(leaky, single_threaded(1));
    CHECK(report.problems.size() == 1);
    if (!report.problems.empty()) {
      CHECK(report.problems[0].description.find("one run ended early") != std::string::npos);
    }
  }
}

TEST(check_determinism_reports_the_same_moves_with_different_random_choices) {
  const auto leaky = [](ravel::Simulation& sim) {
    static int calls = 0;
    if (++calls % 2 == 0) sim.rng().next_below(7);  // Recorded, but invisible in the trace.
    sim.scheduler().spawn("t", [&sim]() -> ravel::Task { co_await sim.scheduler().yield(); });
  };
  const ravel::DeterminismReport report = ravel::check_determinism(leaky, single_threaded(1));
  CHECK(report.problems.size() == 1);
  if (!report.problems.empty()) {
    CHECK(report.problems[0].description.find("different random choices") != std::string::npos);
  }
}

TEST(check_determinism_reports_a_replay_that_goes_its_own_way) {
  // Runs 1 and 2 agree; only the third, the replay, behaves differently.
  const auto leaky = [](ravel::Simulation& sim) {
    static int calls = 0;
    const int nap = (++calls == 3) ? 50 : 5;
    sim.scheduler().spawn("t", [&sim, nap]() -> ravel::Task {
      co_await sim.scheduler().sleep(static_cast<ravel::VirtualClock::Tick>(nap));
    });
  };
  const ravel::DeterminismReport report = ravel::check_determinism(leaky, single_threaded(1));
  CHECK(report.problems.size() == 1);
  if (!report.problems.empty()) {
    CHECK(report.problems[0].description.find("replaying run 1's recorded choices") !=
          std::string::npos);
  }
}

TEST(check_determinism_reports_an_unknown_exception) {
  const auto throws = [](ravel::Simulation&) { throw 42; };
  const ravel::DeterminismReport report = ravel::check_determinism(throws, single_threaded(1));
  CHECK(report.problems.size() == 1);
  if (!report.problems.empty()) {
    CHECK(report.problems[0].description == "the simulation threw an unknown exception");
  }
}
