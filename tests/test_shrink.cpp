#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"
#include "testing.hpp"

namespace {

// Tasks read a counter, yield, and write back read+1, so an interleaving can
// lose an update. The number of tasks is itself a random draw: 2 to 13.
void setup_random_size_lost_update_system(ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);
  const int tasks = 2 + static_cast<int>(sim.rng().next_below(12));
  for (int i = 0; i < tasks; ++i) {
    sim.scheduler().spawn("incrementer", [&sim, &counter]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;
    });
  }
  sim.add_invariant("no_lost_updates", [&counter, tasks] { return counter == tasks; });
}

// The first seed in [0, 500) whose run fails; the tests need a real bug to shrink.
std::uint64_t first_failing_seed(const ravel::SimulationSetup& setup) {
  ravel::RunnerOptions options;
  options.seed_count = 500;
  options.stop_at_first_failure = true;
  const ravel::RunnerReport report = ravel::run_seeds(setup, options);
  return report.failures.empty() ? 0 : report.failures.front().seed;
}

// 40 messages over a lossy link; the invariant demands all arrive.
void setup_lossy_link_system(ravel::Simulation& sim) {
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

bool simpler_or_equal(const ravel::Choices& a, const ravel::Choices& b) {
  if (a.size() != b.size()) return a.size() < b.size();
  return !std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end());
}

}  // namespace

TEST(shrink_finds_a_smaller_run_that_fails_the_same_way) {
  const std::uint64_t seed = first_failing_seed(setup_random_size_lost_update_system);
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_random_size_lost_update_system, seed);

  CHECK(!shrunk.original.ok);
  CHECK(!shrunk.minimal.ok);
  CHECK(shrunk.minimal.failure == shrunk.original.failure);
  CHECK(shrunk.choices.size() <= shrunk.original_choices.size());
  CHECK(simpler_or_equal(shrunk.choices, shrunk.original_choices));
  CHECK(shrunk.minimal.steps <= shrunk.original.steps);
  CHECK(!shrunk.budget_exhausted);
}

TEST(shrink_reduces_the_workload_itself) {
  // Draws made through sim.rng() shrink like any other: the task count falls
  // to its minimum of 2.
  const std::uint64_t seed = first_failing_seed(setup_random_size_lost_update_system);
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_random_size_lost_update_system, seed);

  // Two tasks under plain round-robin already lose an update, so nothing
  // needs recording: an empty list, where every draw takes its default.
  CHECK(shrunk.choices.empty());
  CHECK(shrunk.minimal.steps < shrunk.original.steps);
}

TEST(shrunk_choices_replay_the_minimal_failure) {
  const std::uint64_t seed = first_failing_seed(setup_random_size_lost_update_system);
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_random_size_lost_update_system, seed);

  const ravel::Result replayed =
      ravel::replay(setup_random_size_lost_update_system, shrunk.choices);
  CHECK(!replayed.ok);
  CHECK(replayed.failure == shrunk.original.failure);
  CHECK(replayed.trace_digest == shrunk.minimal.trace_digest);
}

TEST(shrink_gives_the_same_answer_every_time) {
  const std::uint64_t seed = first_failing_seed(setup_random_size_lost_update_system);
  const ravel::ShrinkResult first = ravel::shrink(setup_random_size_lost_update_system, seed);
  const ravel::ShrinkResult second = ravel::shrink(setup_random_size_lost_update_system, seed);
  CHECK(first.choices == second.choices);
  CHECK(first.attempts == second.attempts);
}

TEST(shrink_leaves_a_passing_seed_alone) {
  const auto setup = [](ravel::Simulation&) {};
  const ravel::ShrinkResult shrunk = ravel::shrink(setup, 5);
  CHECK(shrunk.original.ok);
  CHECK(shrunk.minimal.ok);
  CHECK(shrunk.attempts == 0);
}

TEST(shrink_removes_faults_the_failure_does_not_need) {
  // One drop is enough to break the invariant, so the minimal run should have
  // exactly one.
  const std::uint64_t seed = first_failing_seed(setup_lossy_link_system);
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_lossy_link_system, seed);
  CHECK(!shrunk.minimal.ok);

  ravel::Simulation sim(0, ravel::SimulationOptions{.replay_choices = shrunk.choices});
  setup_lossy_link_system(sim);
  sim.run_until_quiescent();
  const auto& events = sim.trace().events();
  const auto drops = std::count_if(events.begin(), events.end(), [](const ravel::TraceEvent& e) {
    return e.kind == ravel::TraceEventKind::MessageDropped;
  });
  CHECK(drops == 1);
}

TEST(shrink_stops_when_the_attempt_budget_is_spent) {
  const std::uint64_t seed = first_failing_seed(setup_lossy_link_system);
  ravel::ShrinkOptions options;
  options.max_attempts = 3;
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_lossy_link_system, seed, options);

  CHECK(shrunk.attempts <= 3);
  CHECK(shrunk.budget_exhausted);
  CHECK(!shrunk.minimal.ok);  // Whatever it reached still fails.
}

TEST(shrink_refuses_a_setup_that_is_not_deterministic) {
  int runs = 0;
  const auto setup = [&runs](ravel::Simulation& sim) {
    const bool fail = runs++ == 0;  // Fails the first time only.
    sim.add_invariant("flaky", [fail] { return !fail; });
  };

  bool threw = false;
  try {
    ravel::shrink(setup, 1);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

TEST(choice_lists_round_trip_through_text) {
  ravel::Choices choices;
  for (std::uint64_t i = 0; i < 40; ++i) choices.push_back(i * i);
  choices.push_back(UINT64_MAX);

  std::stringstream text;
  ravel::write_choices(text, choices);
  CHECK(ravel::read_choices(text) == choices);

  std::stringstream empty;
  ravel::write_choices(empty, {});
  CHECK(ravel::read_choices(empty).empty());
}

TEST(choice_lists_are_written_sixteen_to_a_line) {
  const auto written = [](std::size_t count) {
    ravel::Choices choices;
    for (std::size_t i = 0; i < count; ++i) choices.push_back(i);
    std::ostringstream text;
    ravel::write_choices(text, choices);
    return text.str();
  };
  CHECK(written(0) == "ravel-choices 1\n0\n");
  CHECK(written(1) == "ravel-choices 1\n1\n0\n");
  CHECK(written(16) == "ravel-choices 1\n16\n0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15\n");
  CHECK(written(17) == "ravel-choices 1\n17\n0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15\n16\n");
  CHECK(written(3) == "ravel-choices 1\n3\n0 1 2\n");
}

TEST(choice_list_reader_rejects_bad_input) {
  const auto rejects = [](const std::string& text) {
    std::istringstream in(text);
    try {
      ravel::read_choices(in);
    } catch (const std::runtime_error&) {
      return true;
    }
    return false;
  };
  CHECK(rejects(""));
  CHECK(rejects("something else 1 2 3"));
  CHECK(rejects("ravel-choices 2\n0\n"));      // A version this build does not know.
  CHECK(rejects("ravel-choices 1\n3\n1 2\n"));  // Fewer values than promised.
  CHECK(rejects("ravel-choices 1\n2\n1 x\n"));
}

TEST(shrink_saves_the_minimal_choices_next_to_the_traces) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "ravel-shrink-test";
  std::filesystem::remove_all(dir);

  ravel::ShrinkOptions options;
  options.simulation.trace_dir = dir;
  const std::uint64_t seed = first_failing_seed(setup_lossy_link_system);
  const ravel::ShrinkResult shrunk = ravel::shrink(setup_lossy_link_system, seed, options);

  CHECK(std::filesystem::path(shrunk.choices_path).filename() ==
        "ravel-seed-" + std::to_string(seed) + ".choices");
  CHECK(!shrunk.original.trace_path.empty());
  CHECK(!shrunk.minimal.trace_path.empty());

  ravel::Choices loaded;
  {
    std::ifstream file(shrunk.choices_path);  // Closed before the directory goes.
    loaded = ravel::read_choices(file);
  }
  CHECK(loaded == shrunk.choices);
  CHECK(!ravel::replay(setup_lossy_link_system, loaded).ok);  // A saved repro still fails.

  std::filesystem::remove_all(dir);
}

TEST(runner_can_shrink_the_first_failure) {
  ravel::RunnerOptions options;
  options.seed_count = 200;
  options.shrink_first_failure = true;
  const ravel::RunnerReport report = ravel::run_seeds(setup_lossy_link_system, options);

  CHECK(!report.ok());
  CHECK(report.shrunk.has_value());
  if (report.shrunk) {
    CHECK(report.shrunk->original.seed == report.failures.front().seed);
    CHECK(report.shrunk->choices.size() < report.shrunk->original_choices.size());
  }

  ravel::RunnerOptions no_shrink;
  no_shrink.seed_count = 200;
  CHECK(!ravel::run_seeds(setup_lossy_link_system, no_shrink).shrunk.has_value());
}

TEST(shrink_reports_a_setup_that_throws) {
  const ravel::ShrinkResult named =
      ravel::shrink([](ravel::Simulation&) { throw std::runtime_error("bad setup"); }, 3);
  CHECK(named.original.failure == "simulation threw: bad setup");

  const ravel::ShrinkResult unnamed = ravel::shrink([](ravel::Simulation&) { throw 42; }, 3);
  CHECK(unnamed.original.failure == "simulation threw an unknown exception");
}

TEST(read_choices_rejects_every_kind_of_bad_header) {
  for (const char* text : {"", "ravel-choices", "other-magic 1 0", "ravel-choices 2 0", "ravel-choices 1"}) {
    std::istringstream in(text);
    bool rejected = false;
    try {
      ravel::read_choices(in);
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    CHECK(rejected);
  }
  std::istringstream ok("ravel-choices 1 2 5 7");
  CHECK((ravel::read_choices(ok) == ravel::Choices{5, 7}));
}

TEST(shrink_minimizes_a_run_that_throws_after_drawing_choices) {
  // Throws when its first draw is 7, whatever else it draws. Shrinking must
  // keep the choices made before the throw, or the throw cannot be replayed.
  const auto throws_on_seven = [](ravel::Simulation& sim) {
    const std::uint64_t first = sim.rng().next_between(0, 9);
    sim.rng().next_between(0, 9);
    if (first == 7) throw std::runtime_error("seven");
  };
  std::uint64_t seed = 0;
  while (true) {
    ravel::Simulation probe(seed);
    if (probe.rng().next_between(0, 9) == 7) break;
    ++seed;
  }

  const ravel::ShrinkResult shrunk = ravel::shrink(throws_on_seven, seed);
  CHECK(shrunk.original.failure == "simulation threw: seven");
  CHECK(shrunk.original_choices.size() == 2);
  CHECK(shrunk.minimal.failure == "simulation threw: seven");
  CHECK((shrunk.choices == ravel::Choices{7}));  // The second draw is not needed.
}
