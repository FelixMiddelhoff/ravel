// Pins exactly what the shrinker does (result and number of replays) on a few
// setups of different shapes. The shrinker's answer is any smaller failing
// list, so most edits to its search order still "work"; these tests notice
// them anyway, because a change in the search shows up as a different number
// of attempts. If you change the shrinker on purpose, regenerate the numbers
// by compiling this file with -DRAVEL_PRINT_GOLDEN and review the difference.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"
#include "testing.hpp"

namespace {

// Ten large draws whose total must stay below 5 million: values far apart
// enough that the spread-out search (not one step at a time) is needed.
void setup_big_total(ravel::Simulation& sim) {
  std::uint64_t total = 0;
  for (int i = 0; i < 10; ++i) total += sim.rng().next_below(1000000);
  sim.add_invariant("total_stays_small", [total] { return total < 5000000; });
}

// A run of up to 40 draws where a 9 must not appear after the 15th: stretches
// of choices have to be deleted and zeroed.
void setup_long_run(ravel::Simulation& sim) {
  const std::uint64_t rounds = sim.rng().next_between(0, 40);
  bool bad = false;
  for (std::uint64_t i = 0; i < rounds; ++i) {
    if (sim.rng().next_below(10) == 9 && i >= 15) bad = true;
  }
  sim.add_invariant("no_late_nine", [bad] { return !bad; });
}

// Two draws that must differ by less than 100 (each up to 1000): the pair
// moves must use large steps.
void setup_far_apart(ravel::Simulation& sim) {
  const std::uint64_t first = sim.rng().next_below(1000);
  const std::uint64_t second = sim.rng().next_below(1000);
  const std::uint64_t gap = first > second ? first - second : second - first;
  sim.add_invariant("draws_stay_close", [gap] { return gap < 100; });
}

// Three draws that must not add up to 30 or more, each 0..20.
void setup_three_delays(ravel::Simulation& sim) {
  std::uint64_t total = 0;
  for (int i = 0; i < 3; ++i) total += sim.rng().next_between(0, 20);
  sim.add_invariant("delays_stay_short", [total] { return total < 30; });
}

using Setup = void (*)(ravel::Simulation&);

std::uint64_t first_failing_seed(Setup setup) {
  ravel::RunnerOptions options;
  options.seed_count = 5000;
  options.stop_at_first_failure = true;
  const ravel::RunnerReport report = ravel::run_seeds(setup, options);
  return report.failures.empty() ? 0 : report.failures.front().seed;
}

std::string text_of(const ravel::Choices& choices) {
  std::string text = "{";
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (i != 0) text += ", ";
    text += std::to_string(choices[i]);
  }
  return text + "}";
}

struct Golden {
  const char* name;
  Setup setup;
  unsigned threads;
  std::uint64_t budget;
  std::uint64_t attempts;
  bool exhausted;
  ravel::Choices choices;
};

const std::vector<Golden> kGolden = {
    // GOLDEN-BEGIN
    {"big_total", setup_big_total, 1, 2000, 2000, true, {66420, 335082, 508768, 143532, 723737, 991498, 520344, 127103, 885310, 698206}},
    {"big_total", setup_big_total, 4, 2000, 2000, true, {66420, 335082, 508768, 143532, 723737, 991498, 520344, 127103, 885310, 698206}},
    {"big_total", setup_big_total, 1, 40, 40, true, {66420, 335082, 508768, 143532, 723737, 991498, 520344, 127103, 885617, 740797}},
    {"long_run", setup_long_run, 1, 100000, 217, false, {16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9}},
    {"long_run", setup_long_run, 3, 100000, 217, false, {16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9}},
    {"long_run", setup_long_run, 1, 25, 25, true, {32, 2, 8, 2, 7, 8, 4, 3, 7, 7, 4, 3, 8, 9, 0, 8, 9}},
    {"far_apart", setup_far_apart, 1, 100000, 50, false, {100}},
    {"far_apart", setup_far_apart, 1, 9, 9, true, {120}},
    {"three_delays", setup_three_delays, 1, 100000, 122, false, {10, 20}},
    {"three_delays", setup_three_delays, 2, 100000, 122, false, {10, 20}},
    // GOLDEN-END
};

#ifdef RAVEL_PRINT_GOLDEN
constexpr bool kPrinting = true;
#else
constexpr bool kPrinting = false;
#endif

}  // namespace

TEST(shrink_search_is_pinned_on_setups_of_different_shapes) {
  struct Case {
    const char* name;
    Setup setup;
    unsigned threads;
    std::uint64_t budget;
  };
  const Case cases[] = {
      {"big_total", setup_big_total, 1, 2000},
      {"big_total", setup_big_total, 4, 2000},
      {"big_total", setup_big_total, 1, 40},
      {"long_run", setup_long_run, 1, 100000},
      {"long_run", setup_long_run, 3, 100000},
      {"long_run", setup_long_run, 1, 25},
      {"far_apart", setup_far_apart, 1, 100000},
      {"far_apart", setup_far_apart, 1, 9},
      {"three_delays", setup_three_delays, 1, 100000},
      {"three_delays", setup_three_delays, 2, 100000},
  };
  std::size_t index = 0;
  for (const Case& c : cases) {
    ravel::ShrinkOptions options;
    options.threads = c.threads;
    options.max_attempts = c.budget;
    const ravel::ShrinkResult shrunk = ravel::shrink(c.setup, first_failing_seed(c.setup), options);
    if (kPrinting) {
      std::printf("    {\"%s\", %s, %u, %llu, %llu, %s, %s},\n", c.name,
                  (std::string("setup_") + c.name).c_str(), c.threads,
                  static_cast<unsigned long long>(c.budget),
                  static_cast<unsigned long long>(shrunk.attempts),
                  shrunk.budget_exhausted ? "true" : "false", text_of(shrunk.choices).c_str());
      continue;
    }
    if (index >= kGolden.size()) {
      CHECK(false);
      break;
    }
    const Golden& g = kGolden[index++];
    CHECK(shrunk.attempts == g.attempts);
    CHECK(shrunk.budget_exhausted == g.exhausted);
    CHECK(shrunk.choices == g.choices);
  }
}
