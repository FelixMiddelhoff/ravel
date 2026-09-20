// Properties of ravel itself, checked on many random cases. Each case is drawn
// from a VirtualRng with a fixed seed, so a failure reproduces: the failing
// case number is printed, and the same number always gives the same case.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"
#include "ravel/simulation.hpp"
#include "ravel/trace.hpp"
#include "testing.hpp"

namespace {

// Runs `body(rng, case_number)` for each case and names the case on failure.
template <typename Body>
void for_each_case(std::uint64_t seed, int cases, Body body) {
  ravel::VirtualRng rng(seed);
  for (int i = 0; i < cases; ++i) {
    const int before = ravel::testing::failure_count();
    body(rng, i);
    if (ravel::testing::failure_count() != before) {
      std::printf("    ^ property case %d (generator seed %llu)\n", i,
                  static_cast<unsigned long long>(seed));
      return;
    }
  }
}

std::uint64_t sum_of(const ravel::Choices& choices) {
  return std::accumulate(choices.begin(), choices.end(), std::uint64_t{0});
}

// --- shrink ---------------------------------------------------------------

// `count` draws below `bound`; fails when they add up to `threshold` or more.
ravel::SimulationSetup total_setup(std::uint64_t count, std::uint64_t bound,
                                   std::uint64_t threshold) {
  return [=](ravel::Simulation& sim) {
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < count; ++i) total += sim.rng().next_below(bound);
    sim.add_invariant("total_stays_small", [total, threshold] { return total < threshold; });
  };
}

}  // namespace

TEST(property_shrink_result_fails_the_same_way_and_is_no_bigger) {
  int shrunk_cases = 0;
  for_each_case(101, 60, [&](ravel::VirtualRng& rng, int) {
    const std::uint64_t count = rng.next_between(1, 8);
    const std::uint64_t bound = rng.next_between(2, 1000);
    const std::uint64_t threshold = rng.next_between(1, count * (bound - 1) / 2 + 1);
    const ravel::SimulationSetup setup = total_setup(count, bound, threshold);

    ravel::RunnerOptions runner;
    runner.seed_count = 300;
    runner.threads = 1;
    runner.stop_at_first_failure = true;
    const ravel::RunnerReport report = ravel::run_seeds(setup, runner);
    if (report.failures.empty()) return;
    ++shrunk_cases;

    ravel::ShrinkOptions options;
    options.threads = 1;
    const ravel::ShrinkResult shrunk = ravel::shrink(setup, report.failures[0].seed, options);

    CHECK(!shrunk.minimal.ok);
    CHECK(shrunk.choices.size() <= shrunk.original_choices.size());
    CHECK(sum_of(shrunk.choices) <= sum_of(shrunk.original_choices));

    const ravel::Result again = ravel::replay(setup, shrunk.choices);
    CHECK(!again.ok);
    CHECK(again.failure == shrunk.minimal.failure);
    CHECK(again.trace_digest == shrunk.minimal.trace_digest);
  });
  CHECK(shrunk_cases >= 30);
}

TEST(property_shrink_answer_does_not_depend_on_thread_count) {
  int shrunk_cases = 0;
  for_each_case(102, 25, [&](ravel::VirtualRng& rng, int) {
    const std::uint64_t count = rng.next_between(2, 6);
    const std::uint64_t bound = rng.next_between(10, 500);
    const std::uint64_t threshold = rng.next_between(1, count * (bound - 1) / 2 + 1);
    const ravel::SimulationSetup setup = total_setup(count, bound, threshold);

    ravel::RunnerOptions runner;
    runner.seed_count = 300;
    runner.threads = 1;
    runner.stop_at_first_failure = true;
    const ravel::RunnerReport report = ravel::run_seeds(setup, runner);
    if (report.failures.empty()) return;
    ++shrunk_cases;

    ravel::ShrinkOptions one;
    one.threads = 1;
    ravel::ShrinkOptions many;
    many.threads = static_cast<unsigned>(rng.next_between(2, 5));
    const ravel::ShrinkResult a = ravel::shrink(setup, report.failures[0].seed, one);
    const ravel::ShrinkResult b = ravel::shrink(setup, report.failures[0].seed, many);
    CHECK(a.choices == b.choices);
    CHECK(a.attempts == b.attempts);
  });
  CHECK(shrunk_cases >= 10);
}

// --- replay ---------------------------------------------------------------

namespace {

// Draws a few numbers, sleeps for one of them, and sends over a lossy channel.
void mixed_setup(ravel::Simulation& sim) {
  const std::uint64_t pause = sim.rng().next_below(50);
  const std::uint64_t sends = sim.rng().next_below(6);
  const std::uint64_t wide = sim.rng().next_u64();
  auto& channel = sim.add_channel(
      "a", "b", {.loss_probability = 0.3, .latency_min = 1, .latency_max = 9, .allow_reorder = true});
  sim.scheduler().spawn("sender", [&sim, &channel, pause, sends]() -> ravel::Task {
    for (std::uint64_t i = 0; i < sends; ++i) {
      co_await sim.scheduler().sleep(pause);
      channel.send(std::to_string(i));
    }
  });
  sim.scheduler().spawn("receiver", [&sim, &channel]() -> ravel::Task {
    while (co_await channel.receive_within(200)) {
    }
    co_await sim.scheduler().yield();
  });
  sim.add_invariant("wide_is_not_zero", [wide] { return wide != 0; });
}

}  // namespace

TEST(property_replay_of_any_list_is_deterministic_and_does_not_crash) {
  for_each_case(103, 200, [](ravel::VirtualRng& rng, int) {
    ravel::Choices choices;
    const std::uint64_t length = rng.next_below(25);
    for (std::uint64_t i = 0; i < length; ++i) {
      // Small, huge and in-between values: replay must cope with all of them.
      switch (rng.next_below(3)) {
        case 0: choices.push_back(rng.next_below(4)); break;
        case 1: choices.push_back(rng.next_u64()); break;
        default: choices.push_back(rng.next_below(1000)); break;
      }
    }
    const ravel::Result a = ravel::replay(mixed_setup, choices);
    const ravel::Result b = ravel::replay(mixed_setup, choices);
    CHECK(a.ok == b.ok);
    CHECK(a.steps == b.steps);
    CHECK(a.trace_digest == b.trace_digest);
    CHECK(a.failure == b.failure);
  });
}

TEST(property_recorded_choices_replay_to_the_same_run) {
  for_each_case(104, 60, [](ravel::VirtualRng& rng, int) {
    const std::uint64_t seed = rng.next_u64();
    ravel::Simulation first(seed);
    mixed_setup(first);
    const ravel::Result original = first.run_until_quiescent();
    const ravel::Result replayed = ravel::replay(mixed_setup, first.choices());
    CHECK(replayed.ok == original.ok);
    CHECK(replayed.steps == original.steps);
    CHECK(replayed.trace_digest == original.trace_digest);
  });
}

// --- run_seeds ------------------------------------------------------------

TEST(property_run_seeds_matches_a_plain_loop) {
  for_each_case(105, 25, [](ravel::VirtualRng& rng, int) {
    const std::uint64_t percent = rng.next_between(1, 60);
    const ravel::SimulationSetup setup = [percent](ravel::Simulation& sim) {
      const std::uint64_t roll = sim.rng().next_below(100);
      sim.add_invariant("roll_is_high", [roll, percent] { return roll >= percent; });
    };
    ravel::RunnerOptions options;
    options.first_seed = rng.next_below(1'000'000);
    options.seed_count = rng.next_between(1, 150);
    options.threads = static_cast<unsigned>(rng.next_between(1, 6));
    const ravel::RunnerReport report = ravel::run_seeds(setup, options);

    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::string>> expected;
    for (std::uint64_t s = options.first_seed; s < options.first_seed + options.seed_count; ++s) {
      ravel::Simulation sim(s);
      setup(sim);
      const ravel::Result r = sim.run_until_quiescent();
      if (!r.ok) expected.emplace_back(s, r.trace_digest, r.failure);
    }
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::string>> got;
    for (const ravel::Result& r : report.failures) got.emplace_back(r.seed, r.trace_digest, r.failure);

    CHECK(report.seeds_run == options.seed_count);
    CHECK(got == expected);
  });
}

// --- trace digest ---------------------------------------------------------

namespace {

std::vector<ravel::TraceEvent> random_events(ravel::VirtualRng& rng, std::uint64_t length) {
  std::vector<ravel::TraceEvent> events;
  for (std::uint64_t i = 0; i < length; ++i) {
    events.push_back({rng.next_u64(), static_cast<std::size_t>(rng.next_below(1000)),
                      static_cast<ravel::TraceEventKind>(rng.next_below(11))});
  }
  return events;
}

std::uint64_t digest_of(const std::vector<ravel::TraceEvent>& events) {
  ravel::Trace trace;
  for (const ravel::TraceEvent& event : events) trace.record(event);
  return trace.digest();
}

}  // namespace

TEST(property_trace_digest_changes_exactly_when_the_events_change) {
  for_each_case(106, 300, [](ravel::VirtualRng& rng, int) {
    const std::vector<ravel::TraceEvent> events = random_events(rng, rng.next_between(1, 12));
    CHECK(digest_of(events) == digest_of(std::vector<ravel::TraceEvent>(events)));

    std::vector<ravel::TraceEvent> changed = events;
    const std::size_t at = static_cast<std::size_t>(rng.next_below(changed.size()));
    switch (rng.next_below(5)) {
      case 0: changed[at].time ^= 1ULL << rng.next_below(64); break;
      case 1: changed[at].subject += 1; break;
      case 2:
        changed[at].kind = static_cast<ravel::TraceEventKind>(
            (static_cast<unsigned>(changed[at].kind) + 1 + rng.next_below(10)) % 11);
        break;
      case 3: changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(at)); break;
      default: changed.push_back(changed[at]); break;
    }
    CHECK(changed != events);
    CHECK(digest_of(changed) != digest_of(events));
  });
}

// --- channel --------------------------------------------------------------

TEST(property_channel_keeps_order_and_neither_duplicates_nor_invents_messages) {
  for_each_case(107, 150, [](ravel::VirtualRng& rng, int) {
    const bool lossy = rng.next_below(2) == 0;
    const bool reorder = rng.next_below(2) == 0;
    const ravel::VirtualClock::Tick latency_min = rng.next_below(20);
    const ravel::VirtualClock::Tick latency_max = latency_min + rng.next_below(40);
    const int count = static_cast<int>(rng.next_between(1, 30));

    ravel::Simulation sim(rng.next_u64());
    auto& channel = sim.add_channel("a", "b",
                                    {.loss_probability = lossy ? 0.4 : 0.0,
                                     .latency_min = latency_min,
                                     .latency_max = latency_max,
                                     .allow_reorder = reorder});
    std::vector<int> received;
    sim.scheduler().spawn("sender", [&]() -> ravel::Task {
      for (int i = 0; i < count; ++i) {
        channel.send(std::to_string(i));
        if (i % 3 == 0) co_await sim.scheduler().sleep(static_cast<ravel::VirtualClock::Tick>(i));
      }
    });
    sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
      while (true) received.push_back(std::stoi(co_await channel.receive()));
    });
    sim.run_until_quiescent();

    // Nothing invented, nothing twice.
    std::vector<int> sorted = received;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK(sorted.empty() || (sorted.front() >= 0 && sorted.back() < count));

    if (!lossy) CHECK(static_cast<int>(received.size()) == count);
    if (!reorder) CHECK(std::is_sorted(received.begin(), received.end()));
  });
}

// --- scheduler ------------------------------------------------------------

TEST(property_every_spawned_task_finishes_when_the_run_completes) {
  for_each_case(108, 150, [](ravel::VirtualRng& rng, int) {
    const std::uint64_t tasks = rng.next_between(1, 8);
    ravel::Simulation sim(rng.next_u64());
    for (std::uint64_t t = 0; t < tasks; ++t) {
      const std::uint64_t steps = rng.next_below(9);
      std::vector<std::uint64_t> pauses;
      for (std::uint64_t i = 0; i < steps; ++i) pauses.push_back(rng.next_below(25));
      sim.scheduler().spawn("task", [&sim, pauses]() -> ravel::Task {
        for (const std::uint64_t pause : pauses) {
          if (pause % 2 == 0) {
            co_await sim.scheduler().yield();
          } else {
            co_await sim.scheduler().sleep(pause);
          }
        }
      });
    }
    const ravel::Result result = sim.run_until_quiescent();
    CHECK(result.ok);

    std::uint64_t spawned = 0;
    std::uint64_t finished = 0;
    for (const ravel::TraceEvent& event : sim.trace().events()) {
      if (event.kind == ravel::TraceEventKind::TaskSpawned) ++spawned;
      if (event.kind == ravel::TraceEventKind::TaskFinished) ++finished;
    }
    CHECK(spawned == tasks);
    CHECK(finished == tasks);
  });
}

TEST(property_a_run_that_cannot_finish_is_stopped_by_its_limit) {
  for_each_case(109, 40, [](ravel::VirtualRng& rng, int) {
    ravel::SimulationOptions options;
    options.max_steps = rng.next_between(1, 50);
    ravel::Simulation sim(rng.next_u64(), options);
    sim.scheduler().spawn("spinner", [&sim]() -> ravel::Task {
      while (true) co_await sim.scheduler().yield();
    });
    const ravel::Result result = sim.run_until_quiescent();
    CHECK(!result.ok);
    CHECK(result.steps <= options.max_steps);
  });
}
