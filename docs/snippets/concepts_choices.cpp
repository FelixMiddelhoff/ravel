// A run is its list of choices: print one, replay it, change it.
#include <cstdio>

#include "deposit.hpp"
#include "ravel/shrink.hpp"

namespace {

void print_choices(const char* label, const ravel::Choices& choices) {
  std::printf("%s:", label);
  for (const auto choice : choices) std::printf(" %llu", static_cast<unsigned long long>(choice));
  std::puts("");
}

}  // namespace

// [concepts_main]
int main() {
  const ravel::SimulationSetup setup = deposit_setup(/*dedupe=*/false);

  // 1. Run seed 6 and ask what it decided.
  ravel::Simulation sim(6);
  setup(sim);
  const ravel::Result original = sim.run_until_quiescent();
  print_choices("seed 6 made these choices", sim.choices());
  std::printf("  -> ok=%d, %llu steps, digest %016llx\n", original.ok,
              static_cast<unsigned long long>(original.steps),
              static_cast<unsigned long long>(original.trace_digest));

  // 2. Replay exactly that list. No seed involved: the same run comes back.
  const ravel::Result again = ravel::replay(setup, sim.choices());
  std::printf("replaying the list: digest %016llx (%s)\n",
              static_cast<unsigned long long>(again.trace_digest),
              again.trace_digest == original.trace_digest ? "identical" : "different");

  // 3. Change the list, and you have changed the run: all zeros is the
  //    simplest run there is (first task, no faults, shortest delays).
  const ravel::Result simplest = ravel::replay(setup, ravel::Choices{});
  std::printf("an empty list (all zeros): ok=%d, %llu steps, digest %016llx\n", simplest.ok,
              static_cast<unsigned long long>(simplest.steps),
              static_cast<unsigned long long>(simplest.trace_digest));
  return 0;
}
// [/concepts_main]
