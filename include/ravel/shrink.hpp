#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "ravel/simulation.hpp"

namespace ravel {

using Choices = std::vector<VirtualRng::Choice>;

// Runs the setup once, answering every random draw from `choices` instead of
// a seed. Choices that do not fit are clamped and missing ones are 0, so any
// list is a valid run. Replaying Simulation::choices() reproduces that run.
Result replay(const SimulationSetup& setup, const Choices& choices,
              const SimulationOptions& options = {});

// Writes a choice list as text: a `ravel-choices 1` header, the count, then
// the values. The file is plain and stable, so a minimal reproducer can be
// checked in and replayed as a regression test long after the seed that
// produced it stopped meaning anything.
void write_choices(std::ostream& out, const Choices& choices);

// Reads what write_choices wrote. Throws std::runtime_error if the text is
// not a choice list this version understands.
Choices read_choices(std::istream& in);

struct ShrinkOptions {
  // Replays allowed while shrinking. Shrinking always stops by itself, but on
  // a long failing run it can take many replays to get there.
  std::uint64_t max_attempts = 20'000;

  // Threads that replay candidates at the same time; 0 means one per hardware
  // thread. The result does not depend on this number, only the time it
  // takes. As with run_seeds, the setup then runs on several threads at once,
  // so it must not touch shared mutable state.
  unsigned threads = 0;

  // Applied to every replay. If trace_dir is set, the original failing run
  // and the minimal one each write a trace (the latter as `.replay`), and the
  // minimal choice list is saved there as `ravel-seed-<seed>.choices`.
  SimulationOptions simulation;
};

struct ShrinkResult {
  Result original;             // The seed's own run.
  Choices original_choices;    // Its recorded choices.
  Result minimal;              // The smallest failing run found.
  Choices choices;             // Replay these to reproduce `minimal`.
  std::string choices_path;    // Where `choices` was saved; empty if not.
  std::uint64_t attempts = 0;  // Replays spent shrinking.
  bool budget_exhausted = false;  // Stopped by max_attempts, not by finishing.
};

// Finds a smaller run that fails the same way as `seed`'s run.
//
// A run is fully described by its list of random choices, each a bounded
// integer where 0 is the simplest outcome. Shrinking edits that list (drops
// stretches, zeroes them, lowers single values), replays it, and keeps an
// edit when the run still fails with the same failure and its list is
// shorter, or as long but smaller. Shorter lists mean fewer decisions;
// smaller values mean fewer faults, shorter delays and a more orderly
// schedule. Every random draw counts, including the ones a workload makes
// through Simulation::rng(), so the workload shrinks as well.
//
// The result depends only on the setup and the seed: it is the same on every
// machine. If the seed does not fail, the result reports the passing run
// unchanged, with no attempts made.
//
// Requires a deterministic setup. If replaying the seed's own choices does
// not reproduce its failure (something outside ravel's control leaks into
// the run), this throws std::runtime_error.
ShrinkResult shrink(const SimulationSetup& setup, std::uint64_t seed,
                    const ShrinkOptions& options = {});

}  // namespace ravel
