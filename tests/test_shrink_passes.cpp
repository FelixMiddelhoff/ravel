// The shrinker's finer passes, and its promise that the result does not
// depend on how many threads replay candidates.
#include <cstdint>

#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"
#include "testing.hpp"

namespace {

std::uint64_t first_failing_seed(const ravel::SimulationSetup& setup) {
  ravel::RunnerOptions options;
  options.seed_count = 2000;
  options.stop_at_first_failure = true;
  const ravel::RunnerReport report = ravel::run_seeds(setup, options);
  return report.failures.empty() ? 0 : report.failures.front().seed;
}

ravel::ShrinkOptions with_threads(unsigned threads) {
  ravel::ShrinkOptions options;
  options.threads = threads;
  return options;
}

// Two delays, each 0..20, that must not add up to 25 or more. Lowering either
// one alone stops the failure; the smallest failing list has to trade value
// from the first draw to the second.
void setup_delays_that_add_up(ravel::Simulation& sim) {
  const std::uint64_t first = sim.rng().next_between(0, 20);
  const std::uint64_t second = sim.rng().next_between(0, 20);
  sim.add_invariant("delays_stay_short", [first, second] { return first + second < 25; });
}

// Two draws that fail only when equal (and at least 3). Neither can be lowered
// alone without breaking the tie.
void setup_matching_draws(ravel::Simulation& sim) {
  const std::uint64_t first = sim.rng().next_between(0, 20);
  const std::uint64_t second = sim.rng().next_between(0, 20);
  sim.add_invariant("draws_differ", [first, second] { return !(first == second && first >= 3); });
}

// 40 messages over a lossy link, all of which must arrive: a longer run.
void setup_lossy_link(ravel::Simulation& sim) {
  int& received = sim.make_state<int>(0);
  auto& link = sim.add_channel("a", "b", {.loss_probability = 0.5});
  sim.scheduler().spawn("receiver", [&received, &link]() -> ravel::Task {
    while (true) {
      co_await link.receive();
      ++received;
    }
  });
  sim.scheduler().spawn("sender", [&link]() -> ravel::Task {
    for (int i = 0; i < 40; ++i) link.send("m");
    co_return;
  });
  sim.add_invariant("all_delivered", [&received] { return received == 40; });
}

}  // namespace

TEST(shrink_moves_value_between_draws_that_must_add_up) {
  const std::uint64_t seed = first_failing_seed(setup_delays_that_add_up);
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_delays_that_add_up, seed, with_threads(1));

  // The first delay as small as possible (5), so the second is at its maximum.
  CHECK((shrunk.choices == ravel::Choices{5, 20}));
  CHECK(!shrunk.minimal.ok);
}

TEST(shrink_lowers_two_draws_together) {
  const std::uint64_t seed = first_failing_seed(setup_matching_draws);
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_matching_draws, seed, with_threads(1));
  CHECK((shrunk.choices == ravel::Choices{3, 3}));
}

TEST(shrink_result_does_not_depend_on_the_number_of_threads) {
  for (const auto setup : {setup_delays_that_add_up, setup_matching_draws, setup_lossy_link}) {
    const std::uint64_t seed = first_failing_seed(setup);
    const ravel::ShrinkResult one = ravel::shrink(setup, seed, with_threads(1));
    for (const unsigned threads : {2u, 4u, 7u}) {
      const ravel::ShrinkResult many = ravel::shrink(setup, seed, with_threads(threads));
      CHECK(many.choices == one.choices);
      CHECK(many.attempts == one.attempts);
      CHECK(many.budget_exhausted == one.budget_exhausted);
    }
  }
}

TEST(shrink_budget_is_spent_the_same_way_at_any_thread_count) {
  const std::uint64_t seed = first_failing_seed(setup_lossy_link);
  for (const std::uint64_t budget : {1u, 5u, 12u}) {
    ravel::ShrinkOptions sequential = with_threads(1);
    sequential.max_attempts = budget;
    ravel::ShrinkOptions parallel = with_threads(4);
    parallel.max_attempts = budget;

    const ravel::ShrinkResult a = ravel::shrink(setup_lossy_link, seed, sequential);
    const ravel::ShrinkResult b = ravel::shrink(setup_lossy_link, seed, parallel);
    CHECK(a.attempts <= budget);
    CHECK(a.attempts == b.attempts);
    CHECK(a.choices == b.choices);
    CHECK(a.budget_exhausted == b.budget_exhausted);
  }
}
