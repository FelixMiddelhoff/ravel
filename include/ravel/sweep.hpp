#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "ravel/simulation.hpp"

namespace ravel {

// Defaults for the command line that run_sweep gives your test program. Every
// one can be overridden by a flag (and the first two by an environment
// variable), so the same binary serves a quick local run, a thorough nightly
// one, and the replay of a saved failure.
struct SweepDefaults {
  std::uint64_t seeds = 1000;
  std::string trace_dir = "ravel-traces";  // Where failures leave their trace and reproducer.
  SimulationOptions simulation;            // For example a time_limit.
};

// A ready-made command line for a simulation test program:
//
//   int main(int argc, char** argv) {
//     return ravel::run_sweep_main(argc, argv, my_setup);
//   }
//
// Flags (RAVEL_SEEDS and RAVEL_FIRST_SEED set the first two from the
// environment; a flag beats the variable):
//
//   --seeds N                 how many seeds to run (default: SweepDefaults::seeds)
//   --first-seed S            the first seed (default 0); use it to run a
//                             different slice each night
//   --threads N               worker threads (default: one per hardware thread)
//   --trace-dir DIR           where failures are saved (default: ravel-traces)
//   --no-shrink               do not minimize the first failure
//   --max-shrink-attempts N   budget for shrinking
//   --time-limit TICKS        stop each run at this virtual time
//   --replay FILE             instead of a sweep, replay a saved .choices file
//   --check-determinism       instead of a sweep, check the code is deterministic
//   --help
//
// Exit status: 0 if everything passed, 1 if a seed failed (or a replay
// failed, or determinism was violated), 2 for a bad command line. When
// GITHUB_ACTIONS is set, a failure is also reported as an error annotation.
int run_sweep(std::ostream& out, const std::vector<std::string>& args,
              const SimulationSetup& setup, const SweepDefaults& defaults = {},
              const std::string& program = "test");

int run_sweep_main(int argc, char** argv, const SimulationSetup& setup,
                   const SweepDefaults& defaults = {});

}  // namespace ravel
