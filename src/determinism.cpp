#include "ravel/determinism.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

namespace ravel {

namespace {

// Everything about one run that a second run should reproduce.
struct Observed {
  Result result;
  std::vector<TraceEvent> events;
  std::vector<VirtualRng::Choice> choices;
  std::vector<std::string> event_text;  // describe() of each event, for messages.
};

Observed observe(const SimulationSetup& setup, std::uint64_t seed, SimulationOptions options) {
  Simulation sim(seed, std::move(options));
  setup(sim);

  Observed seen;
  seen.result = sim.run_until_quiescent();
  seen.events = sim.trace().events();
  seen.choices = sim.choices();
  seen.event_text.reserve(seen.events.size());
  for (const TraceEvent& event : seen.events) seen.event_text.push_back(sim.describe(event));
  return seen;
}

// A sentence about where two runs part ways, or nothing if they agree.
std::optional<std::string> difference(const Observed& first, const Observed& second,
                                      const std::string& first_name,
                                      const std::string& second_name) {
  const std::size_t common = std::min(first.events.size(), second.events.size());
  for (std::size_t step = 0; step < common; ++step) {
    if (first.events[step] == second.events[step]) continue;
    return "diverged at step " + std::to_string(step) + ": " + first_name + " " +
           first.event_text[step] + ", " + second_name + " " + second.event_text[step];
  }
  if (first.events.size() != second.events.size()) {
    const bool first_longer = first.events.size() > second.events.size();
    const Observed& longer = first_longer ? first : second;
    return "one run ended early: after step " + std::to_string(common) + " " +
           (first_longer ? second_name : first_name) + " stopped, while " +
           (first_longer ? first_name : second_name) + " went on with " +
           longer.event_text[common];
  }
  if (first.result.failure != second.result.failure) {
    return "the runs made the same moves but " + first_name + " ended with " +
           (first.result.failure.empty() ? "no failure" : "'" + first.result.failure + "'") +
           " and " + second_name + " with " +
           (second.result.failure.empty() ? "no failure" : "'" + second.result.failure + "'") +
           " (an invariant reads something that differs between runs)";
  }
  if (first.choices != second.choices) {
    return "the runs made the same moves but different random choices";
  }
  return std::nullopt;
}

std::optional<std::string> check_seed(const SimulationSetup& setup, std::uint64_t seed,
                                      const SimulationOptions& options) {
  try {
    const Observed first = observe(setup, seed, options);

    const Observed second = observe(setup, seed, options);
    if (const auto problem = difference(first, second, "run 1", "run 2")) {
      return "two runs of the same seed " + *problem;
    }

    SimulationOptions replay = options;
    replay.replay_choices = first.choices;
    const Observed replayed = observe(setup, seed, std::move(replay));
    if (const auto problem = difference(first, replayed, "run 1", "the replay")) {
      return "replaying run 1's recorded choices " + *problem +
             " (something other than ravel's random choices is steering the run)";
    }
  } catch (const std::exception& e) {
    return std::string("the simulation threw: ") + e.what();
  } catch (...) {
    return "the simulation threw an unknown exception";
  }
  return std::nullopt;
}

}  // namespace

DeterminismReport check_determinism(const SimulationSetup& setup,
                                    const DeterminismOptions& options) {
  DeterminismReport report;
  report.seeds_checked = options.seed_count;

  const unsigned wanted =
      options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency());
  const unsigned workers = static_cast<unsigned>(
      std::clamp<std::uint64_t>(wanted, 1, std::max<std::uint64_t>(options.seed_count, 1)));

  std::atomic<std::uint64_t> next_index{0};
  std::mutex problems_mutex;

  const auto work = [&] {
    for (;;) {
      const std::uint64_t index = next_index.fetch_add(1);
      if (index >= options.seed_count) return;

      const std::uint64_t seed = options.first_seed + index;
      if (auto problem = check_seed(setup, seed, options.simulation)) {
        const std::lock_guard<std::mutex> lock(problems_mutex);
        report.problems.push_back({seed, std::move(*problem)});
      }
    }
  };

  if (workers == 1) {
    work();
  } else {
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) threads.emplace_back(work);
    for (std::thread& thread : threads) thread.join();
  }

  std::sort(report.problems.begin(), report.problems.end(),
            [](const DeterminismProblem& a, const DeterminismProblem& b) { return a.seed < b.seed; });
  return report;
}

}  // namespace ravel
