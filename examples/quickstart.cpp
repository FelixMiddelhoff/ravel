// A racy counter: three tasks read the counter, yield, then write back
// read+1. If another task runs in between, an update is lost. Most seeds
// happen to survive; some do not. Any failing seed replays exactly.
#include <cstdint>
#include <cstdio>

#include "ravel/simulation.hpp"

namespace {

ravel::Result run_with_seed(std::uint64_t seed) {
  ravel::Simulation sim(seed);
  int counter = 0;

  for (int i = 0; i < 3; ++i) {
    sim.scheduler().spawn("incrementer", [&]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();
      counter = seen + 1;
    });
  }
  sim.add_invariant("no_lost_updates", [&] { return counter == 3; });

  return sim.run_until_quiescent();
}

}  // namespace

int main() {
  for (std::uint64_t seed = 0; seed < 100; ++seed) {
    const ravel::Result result = run_with_seed(seed);
    if (result.ok) continue;

    std::printf("seed %llu failed: %s\n", static_cast<unsigned long long>(seed),
                result.failure.c_str());

    const ravel::Result replay = run_with_seed(seed);
    std::printf("replay of seed %llu %s\n", static_cast<unsigned long long>(seed),
                replay.trace_digest == result.trace_digest ? "is identical" : "DIVERGED");
    return 1;
  }
  std::puts("no failing seed found");
  return 0;
}
