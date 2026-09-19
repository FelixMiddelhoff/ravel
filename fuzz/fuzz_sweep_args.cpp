// Fuzz target: the command line of run_sweep.
//
// The input is split into arguments at newlines. run_sweep must answer every
// argument list with an exit status and never throw or crash. Numbers are cut to
// two digits and the system under test never fails, so a run stays quick and
// writes no files; what is fuzzed is the parsing, not the simulation.
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "ravel/sweep.hpp"

namespace {

void healthy(ravel::Simulation& sim) {
  sim.scheduler().spawn("t", [&sim]() -> ravel::Task {
    co_await sim.scheduler().sleep(1 + sim.rng().next_below(3));
  });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::vector<std::string> args;
  std::string current;
  for (std::size_t i = 0; i < size && args.size() < 16; ++i) {
    if (data[i] == '\n') {
      args.push_back(current);
      current.clear();
    } else if (current.size() < 64) {
      current += static_cast<char>(data[i]);
    }
  }
  args.push_back(current);

  for (std::string& arg : args) {
    if (!arg.empty() && arg.find_first_not_of("0123456789") == std::string::npos) {
      arg.resize(std::min<std::size_t>(arg.size(), 2));  // Keep runs short.
    }
  }

  ravel::SweepDefaults defaults;
  defaults.seeds = 5;
  std::ostringstream out;
  const int status = ravel::run_sweep(out, args, healthy, defaults, "fuzz");
  if (status < 0 || status > 2) std::abort();
  return 0;
}
