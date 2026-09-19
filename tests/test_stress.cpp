// Stress tests for the thread pools behind run_seeds, shrink and
// check_determinism. The promise under test: the answer never depends on the
// number of threads or on how they happen to be scheduled. Most of the value
// is under ThreadSanitizer (the `thread` CI job), which also sees any data
// race these runs would hide.
//
// RAVEL_STRESS=N multiplies the repetitions (the soak workflow sets it).
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "ravel/determinism.hpp"
#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"
#include "testing.hpp"

namespace {

int stress_scale() {
  const char* text = std::getenv("RAVEL_STRESS");
  const int value = text != nullptr ? std::atoi(text) : 1;
  return value > 0 ? value : 1;
}

const unsigned kThreadCounts[] = {1, 2, 3, 4, 7, 16, 32};

// Fails on some seeds: a lost update among a random number of tasks.
void lost_update(ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);
  const int tasks = 2 + static_cast<int>(sim.rng().next_below(4));
  for (int i = 0; i < tasks; ++i) {
    sim.scheduler().spawn("incrementer", [&sim, &counter]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;
    });
  }
  sim.add_invariant("no_lost_updates", [&counter, tasks] { return counter == tasks; });
}

// A long failing run: 30 messages over a lossy link that must all arrive.
void lossy_link(ravel::Simulation& sim) {
  int& received = sim.make_state<int>(0);
  auto& link = sim.add_channel("a", "b", {.loss_probability = 0.5});
  sim.scheduler().spawn("receiver", [&received, &link]() -> ravel::Task {
    while (true) {
      co_await link.receive();
      ++received;
    }
  });
  sim.scheduler().spawn("sender", [&link]() -> ravel::Task {
    for (int i = 0; i < 30; ++i) link.send("m");
    co_return;
  });
  sim.add_invariant("all_delivered", [&received] { return received == 30; });
}

// Throws for some choice lists and fails for others, so failures of two kinds
// mix while shrinking.
void throws_and_fails(ravel::Simulation& sim) {
  const std::uint64_t a = sim.rng().next_between(0, 9);
  const std::uint64_t b = sim.rng().next_between(0, 9);
  if (a == 7) throw std::runtime_error("seven");
  sim.add_invariant("sum_small", [a, b] { return a + b < 12; });
}

// Fails whatever the choices are, with no choices at all.
void always_fails(ravel::Simulation& sim) {
  sim.add_invariant("never_holds", [] { return false; });
}

// Fails as soon as its only draw is at least 1.
void one_draw(ravel::Simulation& sim) {
  const std::uint64_t value = sim.rng().next_below(4);
  sim.add_invariant("zero_only", [value] { return value == 0; });
}

// Throws on a seed-dependent third of its runs, fails on another sixth.
void seed_dependent(ravel::Simulation& sim) {
  const std::uint64_t roll = sim.rng().next_below(6);
  if (roll == 0 || roll == 1) throw std::runtime_error("bad luck");
  sim.add_invariant("not_five", [roll] { return roll != 5; });
}

bool same(const ravel::Result& a, const ravel::Result& b) {
  return a.seed == b.seed && a.ok == b.ok && a.failure == b.failure && a.steps == b.steps &&
         a.trace_digest == b.trace_digest;
}

bool same(const ravel::RunnerReport& a, const ravel::RunnerReport& b) {
  if (a.seeds_run != b.seeds_run || a.failures.size() != b.failures.size()) return false;
  for (std::size_t i = 0; i < a.failures.size(); ++i) {
    if (!same(a.failures[i], b.failures[i])) return false;
  }
  return true;
}

std::uint64_t first_failing_seed(const ravel::SimulationSetup& setup) {
  ravel::RunnerOptions options;
  options.seed_count = 2000;
  options.stop_at_first_failure = true;
  options.threads = 1;
  const ravel::RunnerReport report = ravel::run_seeds(setup, options);
  return report.failures.empty() ? 0 : report.failures.front().seed;
}

}  // namespace

TEST(stress_run_seeds_reports_the_same_whatever_the_thread_count) {
  const ravel::SimulationSetup setups[] = {lost_update, lossy_link, seed_dependent, always_fails};
  for (const auto& setup : setups) {
    for (const std::uint64_t count : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{2},
                                      std::uint64_t{7}, std::uint64_t{150}}) {
      for (const bool stop_at_first : {false, true}) {
        ravel::RunnerOptions options;
        options.first_seed = 5;
        options.seed_count = count;
        options.stop_at_first_failure = stop_at_first;
        options.threads = 1;
        const ravel::RunnerReport expected = ravel::run_seeds(setup, options);

        for (const unsigned threads : kThreadCounts) {
          options.threads = threads;
          for (int repeat = 0; repeat < 2 * stress_scale(); ++repeat) {
            CHECK(same(ravel::run_seeds(setup, options), expected));
          }
        }
      }
    }
  }
}

TEST(stress_shrink_gives_the_same_answer_whatever_the_thread_count) {
  const ravel::SimulationSetup setups[] = {lost_update, lossy_link, throws_and_fails, always_fails,
                                           one_draw};
  for (const auto& setup : setups) {
    const std::uint64_t seed = first_failing_seed(setup);
    ravel::ShrinkOptions options;
    options.threads = 1;
    const ravel::ShrinkResult expected = ravel::shrink(setup, seed, options);
    CHECK(!expected.minimal.ok);

    for (const unsigned threads : kThreadCounts) {
      options.threads = threads;
      for (int repeat = 0; repeat < stress_scale(); ++repeat) {
        const ravel::ShrinkResult got = ravel::shrink(setup, seed, options);
        CHECK(got.choices == expected.choices);
        CHECK(got.attempts == expected.attempts);
        CHECK(same(got.minimal, expected.minimal));
        CHECK(got.budget_exhausted == expected.budget_exhausted);
      }
    }
  }
}

TEST(stress_shrink_with_a_tiny_budget_is_also_thread_independent) {
  const std::uint64_t seed = first_failing_seed(lossy_link);
  for (const std::uint64_t budget : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{5},
                                     std::uint64_t{17}}) {
    ravel::ShrinkOptions options;
    options.max_attempts = budget;
    options.threads = 1;
    const ravel::ShrinkResult expected = ravel::shrink(lossy_link, seed, options);
    for (const unsigned threads : {2u, 5u, 32u}) {
      options.threads = threads;
      const ravel::ShrinkResult got = ravel::shrink(lossy_link, seed, options);
      CHECK(got.choices == expected.choices);
      CHECK(got.attempts == expected.attempts);
      CHECK(got.budget_exhausted == expected.budget_exhausted);
    }
  }
}

TEST(stress_check_determinism_reports_the_same_whatever_the_thread_count) {
  const ravel::SimulationSetup setups[] = {lost_update, lossy_link, seed_dependent};
  for (const auto& setup : setups) {
    ravel::DeterminismOptions options;
    options.seed_count = 60;
    options.threads = 1;
    const ravel::DeterminismReport expected = ravel::check_determinism(setup, options);

    for (const unsigned threads : kThreadCounts) {
      options.threads = threads;
      for (int repeat = 0; repeat < stress_scale(); ++repeat) {
        const ravel::DeterminismReport got = ravel::check_determinism(setup, options);
        CHECK(got.seeds_checked == expected.seeds_checked);
        CHECK(got.problems.size() == expected.problems.size());
        for (std::size_t i = 0; i < got.problems.size() && i < expected.problems.size(); ++i) {
          CHECK(got.problems[i].seed == expected.problems[i].seed);
          CHECK(got.problems[i].description == expected.problems[i].description);
        }
      }
    }
  }
}

TEST(stress_many_small_pools_in_a_row) {
  // A pool is created and torn down per shrink; hammer that.
  const std::uint64_t seed = first_failing_seed(one_draw);
  for (int i = 0; i < 40 * stress_scale(); ++i) {
    ravel::ShrinkOptions options;
    options.threads = 2 + static_cast<unsigned>(i % 9);
    const ravel::ShrinkResult result = ravel::shrink(one_draw, seed, options);
    CHECK(result.choices == ravel::Choices{1});
  }
}
