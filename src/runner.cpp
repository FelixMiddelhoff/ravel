#include "ravel/runner.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace ravel {

namespace {

Result run_one_seed(std::uint64_t seed, const SimulationSetup& setup,
                    const SimulationOptions& options) {
  try {
    Simulation sim(seed, options);
    setup(sim);
    return sim.run_until_quiescent();
  } catch (const std::exception& e) {
    return Result{.seed = seed, .failure = std::string("simulation threw: ") + e.what()};
  } catch (...) {
    return Result{.seed = seed, .failure = "simulation threw an unknown exception"};
  }
}

unsigned worker_count(unsigned requested, std::uint64_t seed_count) {
  const std::uint64_t wanted = requested != 0 ? requested : std::thread::hardware_concurrency();
  const std::uint64_t useful = std::max<std::uint64_t>(seed_count, 1);  // No idle threads.
  return static_cast<unsigned>(std::clamp<std::uint64_t>(wanted, 1, useful));
}

// State shared by the workers of one run_seeds call.
class SeedQueue {
 public:
  SeedQueue(const SimulationSetup& setup, const RunnerOptions& options)
      : setup_(setup), options_(options) {}

  // Each worker repeatedly claims the next unclaimed seed and runs it.
  void work() {
    while (true) {
      const std::uint64_t index = next_index_.fetch_add(1);
      if (index >= options_.seed_count) return;
      // Seeds are claimed in ascending order, so everything below a known
      // failure has already been claimed and will finish. Later ones are moot.
      if (options_.stop_at_first_failure && index > first_failure_index_.load()) return;

      Result result = run_one_seed(options_.first_seed + index, setup_, options_.simulation);
      if (!result.ok) record_failure(index, std::move(result));
    }
  }

  RunnerReport finish() {
    RunnerReport report;
    report.seeds_run = options_.seed_count;
    report.failures = std::move(failures_);
    std::sort(report.failures.begin(), report.failures.end(),
              [](const Result& a, const Result& b) { return a.seed < b.seed; });

    if (options_.stop_at_first_failure && !report.failures.empty()) {
      report.failures.resize(1);
      report.seeds_run = report.failures.front().seed - options_.first_seed + 1;
    }
    return report;
  }

 private:
  void record_failure(std::uint64_t index, Result result) {
    {
      const std::lock_guard<std::mutex> lock(failures_mutex_);
      failures_.push_back(std::move(result));
    }
    // Lower first_failure_index_ to `index` if it is lower than the current one.
    std::uint64_t known = first_failure_index_.load();
    while (index < known && !first_failure_index_.compare_exchange_weak(known, index)) {
    }
  }

  const SimulationSetup& setup_;
  const RunnerOptions& options_;

  std::atomic<std::uint64_t> next_index_{0};
  std::atomic<std::uint64_t> first_failure_index_{std::numeric_limits<std::uint64_t>::max()};
  std::mutex failures_mutex_;
  std::vector<Result> failures_;
};

}  // namespace

RunnerReport run_seeds(const SimulationSetup& setup, const RunnerOptions& options) {
  SeedQueue queue(setup, options);
  const unsigned workers = worker_count(options.threads, options.seed_count);

  if (workers == 1) {
    queue.work();  // No need for a thread; also easier to debug.
  } else {
    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) threads.emplace_back([&queue] { queue.work(); });
    for (std::thread& thread : threads) thread.join();
  }
  RunnerReport report = queue.finish();

  if (options.shrink_first_failure && !report.failures.empty()) {
    ShrinkOptions shrink_options;
    shrink_options.max_attempts = options.max_shrink_attempts;
    shrink_options.threads = options.threads;
    shrink_options.simulation = options.simulation;
    report.shrunk = shrink(setup, report.failures.front().seed, shrink_options);
  }
  return report;
}

}  // namespace ravel
