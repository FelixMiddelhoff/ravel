#include "ravel/ravel.h"

#include <new>

#include "ravel/simulation.hpp"
#include "ravel/version.hpp"

struct ravel_simulation {
  explicit ravel_simulation(std::uint64_t seed) : simulation(seed) {}
  ravel::Simulation simulation;
};

extern "C" {

ravel_simulation* ravel_simulation_create(uint64_t seed) {
  return new (std::nothrow) ravel_simulation(seed);
}

void ravel_simulation_destroy(ravel_simulation* sim) { delete sim; }

int ravel_simulation_run(ravel_simulation* sim) {
  if (sim == nullptr) return 0;
  try {
    return sim->simulation.run_until_quiescent().ok ? 1 : 0;
  } catch (...) {
    return 0;  // No exception may cross the C boundary.
  }
}

const char* ravel_version_string() { return ravel::version_string(); }

}  // extern "C"
