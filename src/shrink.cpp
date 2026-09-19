#include "ravel/shrink.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>

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

class Shrinker {
 public:
  Shrinker(const SimulationSetup& setup, std::uint64_t seed, const ShrinkOptions& options,
           Choices start, std::string failure)
      : setup_(setup),
        seed_(seed),
        max_attempts_(options.max_attempts),
        probe_options_(options.simulation),
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
    }
  }

  const Choices& best() const noexcept { return best_; }
  std::uint64_t attempts() const noexcept { return attempts_; }
  bool exhausted() const noexcept { return exhausted_; }

 private:
  // Replays `candidate`. Adopts what actually ran (which may be shorter than
  // the candidate) if it fails the same way and is simpler than the best.
  bool try_candidate(const Choices& candidate) {
    if (attempts_ >= max_attempts_) {
      exhausted_ = true;
      return false;
    }
    ++attempts_;

    Run run = run_once(setup_, seed_, probe_options_, candidate);
    if (run.result.ok || run.result.failure != failure_) return false;

    drop_trailing_zeros(run.choices);
    if (!simpler_than(run.choices, best_)) return false;

    best_ = std::move(run.choices);
    return true;
  }

  // Tries removing each stretch of choices, largest stretches first.
  bool delete_stretches() {
    bool progress = false;
    for (const std::size_t size : chunk_sizes(best_.size())) {
      std::size_t start = best_.size() >= size ? best_.size() - size : 0;
      while (size <= best_.size() && !exhausted_) {
        start = std::min(start, best_.size() - size);
        Choices candidate = best_;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(start),
                        candidate.begin() + static_cast<std::ptrdiff_t>(start + size));
        if (try_candidate(candidate)) {
          progress = true;  // Same spot again: the list changed under it.
        } else if (start == 0) {
          break;
        } else {
          --start;
        }
      }
    }
    return progress;
  }

  // Tries setting each stretch of choices to 0, largest stretches first.
  bool zero_stretches() {
    bool progress = false;
    for (const std::size_t size : chunk_sizes(best_.size())) {
      for (std::size_t start = best_.size() >= size ? best_.size() - size + 1 : 0;
           start-- > 0 && !exhausted_;) {
        if (start + size > best_.size()) continue;
        Choices candidate = best_;
        const auto first = candidate.begin() + static_cast<std::ptrdiff_t>(start);
        if (std::all_of(first, first + static_cast<std::ptrdiff_t>(size),
                        [](VirtualRng::Choice c) { return c == 0; })) {
          continue;  // Nothing to change.
        }
        std::fill(first, first + static_cast<std::ptrdiff_t>(size), 0);
        progress |= try_candidate(candidate);
      }
    }
    return progress;
  }

  // Lowers each choice as far as the failure survives, by bisection.
  bool lower_values() {
    bool progress = false;
    for (std::size_t i = best_.size(); i-- > 0 && !exhausted_;) {
      if (i >= best_.size() || best_[i] == 0) continue;

      VirtualRng::Choice low = 0;  // Assumed to pass (zeroing was tried).
      while (i < best_.size() && best_[i] - low > 1 && !exhausted_) {
        const VirtualRng::Choice middle = low + (best_[i] - low) / 2;
        Choices candidate = best_;
        candidate[i] = middle;
        if (try_candidate(candidate)) {
          progress = true;
        } else {
          low = middle;
        }
      }
    }
    return progress;
  }

  const SimulationSetup& setup_;
  std::uint64_t seed_;
  std::uint64_t max_attempts_;
  SimulationOptions probe_options_;

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
