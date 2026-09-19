#include "ravel/shrink.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <functional>
#include <istream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace ravel {

namespace {

// One run of the setup, with the choices it actually made.
struct Run {
  Result result;
  Choices choices;
};

Run run_once(const SimulationSetup& setup, std::uint64_t seed, SimulationOptions options,
             std::optional<Choices> replay_choices) {
  options.replay_choices = std::move(replay_choices);
  try {
    Simulation sim(seed, std::move(options));
    setup(sim);
    Result result = sim.run_until_quiescent();
    return {std::move(result), sim.choices()};
  } catch (const std::exception& e) {
    return {Result{.seed = seed, .failure = std::string("simulation threw: ") + e.what()}, {}};
  } catch (...) {
    return {Result{.seed = seed, .failure = "simulation threw an unknown exception"}, {}};
  }
}

// Trailing zeros are what a replay supplies anyway, so they carry no
// information.
void drop_trailing_zeros(Choices& choices) {
  while (!choices.empty() && choices.back() == 0) choices.pop_back();
}

// The order shrinking minimizes: shorter first, then smaller values.
bool simpler_than(const Choices& a, const Choices& b) {
  if (a.size() != b.size()) return a.size() < b.size();
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

// The sizes n, n/2, n/4, ... 1: big cuts first, then finer ones.
std::vector<std::size_t> chunk_sizes(std::size_t n) {
  std::vector<std::size_t> sizes;
  for (std::size_t size = n; size >= 1; size /= 2) sizes.push_back(size);
  return sizes;
}

// Saves the list into `dir`, if one is set. Returns the file's path, or an
// empty string if nothing was written.
std::string save_choices(const std::filesystem::path& dir, std::uint64_t seed,
                         const Choices& choices) {
  if (dir.empty()) return {};

  std::error_code error;
  std::filesystem::create_directories(dir, error);
  const auto path = dir / ("ravel-seed-" + std::to_string(seed) + ".choices");
  std::ofstream file(path);
  if (error || !file) return {};

  write_choices(file, choices);
  file.flush();
  return file ? path.string() : std::string();
}

// Runs a batch of independent jobs on a fixed set of threads, and reports the
// lowest-numbered job that succeeded. Jobs numbered above a success are
// skipped where possible, and the answer does not depend on how many threads
// there are or how they were scheduled.
class WorkerPool {
 public:
  // `threads` counts the caller, which works too, so this starts threads - 1.
  explicit WorkerPool(unsigned threads) {
    for (unsigned i = 1; i < threads; ++i) helpers_.emplace_back([this] { help(); });
  }

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  ~WorkerPool() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& helper : helpers_) helper.join();
  }

  // Calls job(0), job(1), ... job(count - 1), possibly at the same time, and
  // returns the lowest index whose job returned true, or `count` if none did.
  std::size_t find_first(std::size_t count, const std::function<bool(std::size_t)>& job) {
    if (helpers_.empty() || count <= 1) {
      for (std::size_t i = 0; i < count; ++i) {
        if (job(i)) return i;
      }
      return count;
    }

    Batch batch{count, &job};
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      batch_ = &batch;
      batch.helpers_left = helpers_.size();
      ++generation_;
    }
    wake_.notify_all();

    work_on(batch);

    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [&batch] { return batch.helpers_left == 0; });
    batch_ = nullptr;
    return batch.lowest_success.load();
  }

 private:
  struct Batch {
    Batch(std::size_t job_count, const std::function<bool(std::size_t)>* job_function)
        : count(job_count), job(job_function), lowest_success(job_count) {}

    const std::size_t count;
    const std::function<bool(std::size_t)>* job;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> lowest_success;
    std::size_t helpers_left = 0;  // Guarded by the pool's mutex.
  };

  static void work_on(Batch& batch) {
    for (;;) {
      const std::size_t index = batch.next.fetch_add(1);
      // Everything below a success has been claimed already and will finish;
      // anything above it cannot change the answer.
      if (index >= batch.count || index > batch.lowest_success.load()) return;
      if (!(*batch.job)(index)) continue;

      std::size_t known = batch.lowest_success.load();
      while (index < known && !batch.lowest_success.compare_exchange_weak(known, index)) {
      }
    }
  }

  void help() {
    std::uint64_t seen = 0;
    for (;;) {
      Batch* batch = nullptr;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
        if (stopping_) return;
        seen = generation_;
        batch = batch_;
      }

      work_on(*batch);

      const std::lock_guard<std::mutex> lock(mutex_);
      if (--batch->helpers_left == 0) done_.notify_one();
    }
  }

  std::vector<std::thread> helpers_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable done_;
  Batch* batch_ = nullptr;
  std::uint64_t generation_ = 0;
  bool stopping_ = false;
};

unsigned thread_count(unsigned requested) {
  return requested != 0 ? requested : std::max(1u, std::thread::hardware_concurrency());
}

// Candidates are tried a batch at a time, so replays can run in parallel.
constexpr std::size_t kBatch = 16;

// How far ahead of a value the pair passes look for a partner.
constexpr std::size_t kPairWindow = 6;

class Shrinker {
 public:
  Shrinker(const SimulationSetup& setup, std::uint64_t seed, const ShrinkOptions& options,
           Choices start, std::string failure)
      : setup_(setup),
        seed_(seed),
        max_attempts_(options.max_attempts),
        probe_options_(options.simulation),
        pool_(thread_count(options.threads)),
        best_(std::move(start)),
        failure_(std::move(failure)) {
    // Attempts only need the verdict, never a trace file.
    probe_options_.trace_dir.clear();
  }

  // Runs passes until one full round changes nothing, or the budget is spent.
  void run() {
    bool progress = true;
    while (progress && !exhausted_) {
      progress = delete_stretches();
      progress |= zero_stretches();
      progress |= lower_values();
      progress |= lower_pairs();
    }
  }

  const Choices& best() const noexcept { return best_; }
  std::uint64_t attempts() const noexcept { return attempts_; }
  bool exhausted() const noexcept { return exhausted_; }

 private:
  // Replays `candidate`. True if it fails the same way and is simpler than the
  // best so far, with what actually ran (which may be shorter than the
  // candidate) in `adopted`. Safe to call from several threads: it only reads.
  bool improves(const Choices& candidate, Choices& adopted) const {
    Run run = run_once(setup_, seed_, probe_options_, candidate);
    if (run.result.ok || run.result.failure != failure_) return false;

    drop_trailing_zeros(run.choices);
    if (!simpler_than(run.choices, best_)) return false;

    adopted = std::move(run.choices);
    return true;
  }

  // Replays the candidates in order and adopts the first improving one. Costs
  // (in attempts) exactly the candidates up to and including it, as if they
  // had been tried one by one, whatever the thread count. Returns its index,
  // or nothing if none improves or the budget ran out first.
  std::optional<std::size_t> adopt_first_improving(const std::vector<Choices>& candidates) {
    const std::uint64_t remaining = max_attempts_ - attempts_;
    const std::size_t considered =
        static_cast<std::size_t>(std::min<std::uint64_t>(candidates.size(), remaining));

    std::vector<Choices> adopted(considered);
    const std::size_t found = pool_.find_first(
        considered, [&](std::size_t i) { return improves(candidates[i], adopted[i]); });

    if (found < considered) {
      attempts_ += found + 1;
      best_ = std::move(adopted[found]);
      return found;
    }
    attempts_ += considered;
    if (considered < candidates.size()) exhausted_ = true;
    return std::nullopt;
  }

  // Tries removing each stretch of choices, largest stretches first.
  bool delete_stretches() {
    bool progress = false;
    for (const std::size_t size : chunk_sizes(best_.size())) {
      std::size_t start = best_.size() >= size ? best_.size() - size : 0;
      while (size <= best_.size() && !exhausted_) {
        start = std::min(start, best_.size() - size);

        // Removing the stretch at `start`, and at each spot below it.
        const std::size_t count = std::min(kBatch, start + 1);
        std::vector<Choices> batch;
        for (std::size_t k = 0; k < count; ++k) {
          Choices candidate = best_;
          const auto first = candidate.begin() + static_cast<std::ptrdiff_t>(start - k);
          candidate.erase(first, first + static_cast<std::ptrdiff_t>(size));
          batch.push_back(std::move(candidate));
        }

        if (const auto hit = adopt_first_improving(batch)) {
          progress = true;
          start -= *hit;  // Same spot again: the list changed under it.
        } else if (count == start + 1) {
          break;  // Reached the front.
        } else {
          start -= count;
        }
      }
    }
    return progress;
  }

  // Tries setting each stretch of choices to 0, largest stretches first.
  bool zero_stretches() {
    bool progress = false;
    for (const std::size_t size : chunk_sizes(best_.size())) {
      // Stretches start below `bound`; each round looks at the next few.
      std::size_t bound = best_.size() >= size ? best_.size() - size + 1 : 0;
      while (bound > 0 && !exhausted_) {
        std::vector<Choices> batch;
        std::vector<std::size_t> starts;
        std::size_t cursor = bound;
        while (cursor > 0 && batch.size() < kBatch) {
          --cursor;
          if (cursor + size > best_.size()) continue;
          const auto first = best_.begin() + static_cast<std::ptrdiff_t>(cursor);
          const auto last = first + static_cast<std::ptrdiff_t>(size);
          if (std::all_of(first, last, [](VirtualRng::Choice c) { return c == 0; })) continue;

          Choices candidate = best_;
          std::fill(candidate.begin() + static_cast<std::ptrdiff_t>(cursor),
                    candidate.begin() + static_cast<std::ptrdiff_t>(cursor + size), 0);
          batch.push_back(std::move(candidate));
          starts.push_back(cursor);
        }
        if (batch.empty()) break;

        if (const auto hit = adopt_first_improving(batch)) {
          progress = true;
          bound = starts[*hit];  // Carry on below the stretch that worked.
        } else {
          bound = cursor;
        }
      }
    }
    return progress;
  }

  // The values strictly between `low` and `high`, ascending: all of them if
  // there are few, otherwise kBatch spread evenly.
  static std::vector<VirtualRng::Choice> values_between(VirtualRng::Choice low,
                                                        VirtualRng::Choice high) {
    std::vector<VirtualRng::Choice> values;
    const VirtualRng::Choice gap = high - low;  // At least 2 here.
    if (gap - 1 <= kBatch) {
      for (VirtualRng::Choice v = low + 1; v < high; ++v) values.push_back(v);
    } else {
      const VirtualRng::Choice step = gap / (kBatch + 1);
      for (std::size_t k = 1; k <= kBatch; ++k) values.push_back(low + step * k);
    }
    return values;
  }

  // Lowers each choice as far as the failure survives. Each round tries a
  // spread of smaller values at once, assuming that if a value fails to
  // reproduce the failure then so do the ones below it.
  bool lower_values() {
    bool progress = false;
    for (std::size_t i = best_.size(); i-- > 0 && !exhausted_;) {
      if (i >= best_.size() || best_[i] == 0) continue;

      VirtualRng::Choice low = 0;  // Zeroing was tried already.
      while (i < best_.size() && best_[i] > low + 1 && !exhausted_) {
        const std::vector<VirtualRng::Choice> values = values_between(low, best_[i]);
        std::vector<Choices> batch;
        for (const VirtualRng::Choice value : values) {
          Choices candidate = best_;
          candidate[i] = value;
          batch.push_back(std::move(candidate));
        }

        if (const auto hit = adopt_first_improving(batch)) {
          progress = true;
          if (*hit > 0) low = values[*hit - 1];  // The smaller ones failed.
        } else {
          low = values.back();
        }
      }
    }
    return progress;
  }

  // Lowers two choices together. Some failures need two draws to add up to
  // something (two delays, say), so neither can drop alone. Tried for each
  // choice and its next few neighbours:
  //   * lower both by the same amount;
  //   * move all of the earlier one's value into the later one, or one unit of
  //     it. The list gets smaller because the earlier value falls.
  bool lower_pairs() {
    bool progress = false;
    for (std::size_t i = best_.size(); i-- > 0 && !exhausted_;) {
      while (i < best_.size() && best_[i] != 0 && !exhausted_) {
        std::vector<Choices> batch;
        const std::size_t last = std::min(best_.size() - 1, i + kPairWindow);
        for (std::size_t j = i + 1; j <= last; ++j) {
          const VirtualRng::Choice a = best_[i];
          const VirtualRng::Choice b = best_[j];

          const auto add = [&](VirtualRng::Choice new_a, VirtualRng::Choice new_b) {
            Choices candidate = best_;
            candidate[i] = new_a;
            candidate[j] = new_b;
            batch.push_back(std::move(candidate));
          };

          if (b != 0) {
            const VirtualRng::Choice common = std::min(a, b);
            add(a - common, b - common);
            if (common > 1) add(a - 1, b - 1);
          }
          if (a + b >= a) add(0, a + b);  // Skipped on overflow.
          if (b + 1 != 0) add(a - 1, b + 1);
        }
        if (batch.empty() || !adopt_first_improving(batch)) break;
        progress = true;  // Something changed: look at this spot again.
      }
    }
    return progress;
  }

  const SimulationSetup& setup_;
  std::uint64_t seed_;
  std::uint64_t max_attempts_;
  SimulationOptions probe_options_;
  WorkerPool pool_;

  Choices best_;
  std::string failure_;  // The failure every accepted run must reproduce.
  std::uint64_t attempts_ = 0;
  bool exhausted_ = false;
};

}  // namespace

void write_choices(std::ostream& out, const Choices& choices) {
  constexpr std::size_t kPerLine = 16;
  out << "ravel-choices 1\n" << choices.size() << "\n";
  for (std::size_t i = 0; i < choices.size(); ++i) {
    const bool ends_line = (i + 1) % kPerLine == 0 || i + 1 == choices.size();
    out << choices[i] << (ends_line ? '\n' : ' ');
  }
}

Choices read_choices(std::istream& in) {
  std::string magic;
  int version = 0;
  std::size_t count = 0;
  if (!(in >> magic >> version >> count) || magic != "ravel-choices" || version != 1) {
    throw std::runtime_error("not a ravel choice list (expected header 'ravel-choices 1')");
  }

  Choices choices;
  for (std::size_t i = 0; i < count; ++i) {
    VirtualRng::Choice choice = 0;
    if (!(in >> choice)) throw std::runtime_error("ravel choice list is truncated or malformed");
    choices.push_back(choice);
  }
  return choices;
}

Result replay(const SimulationSetup& setup, const Choices& choices,
              const SimulationOptions& options) {
  return run_once(setup, /*seed=*/0, options, choices).result;
}

ShrinkResult shrink(const SimulationSetup& setup, std::uint64_t seed,
                    const ShrinkOptions& options) {
  Run original = run_once(setup, seed, options.simulation, std::nullopt);

  ShrinkResult shrunk;
  shrunk.original = original.result;
  shrunk.original_choices = original.choices;
  shrunk.minimal = original.result;
  shrunk.choices = original.choices;
  if (original.result.ok) return shrunk;

  Choices start = original.choices;
  drop_trailing_zeros(start);

  // Shrinking trusts that a list of choices reproduces the run.
  const Result check = run_once(setup, seed, {}, start).result;
  if (check.ok || check.failure != original.result.failure) {
    throw std::runtime_error(
        "ravel::shrink: replaying seed " + std::to_string(seed) +
        " did not reproduce its failure; the setup is not deterministic");
  }

  Shrinker shrinker(setup, seed, options, std::move(start), original.result.failure);
  shrinker.run();

  shrunk.choices = shrinker.best();
  shrunk.attempts = shrinker.attempts();
  shrunk.budget_exhausted = shrinker.exhausted();

  // Run the winner once more with the caller's options, so the minimal
  // result carries its trace path and step count.
  shrunk.minimal = run_once(setup, seed, options.simulation, shrunk.choices).result;
  shrunk.choices_path = save_choices(options.simulation.trace_dir, seed, shrunk.choices);
  return shrunk;
}

}  // namespace ravel
