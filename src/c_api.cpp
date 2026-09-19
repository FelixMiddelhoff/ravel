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
  try {
    // std::nothrow only covers the allocation; the constructor can still throw.
    return new (std::nothrow) ravel_simulation(seed);
  } catch (...) {
    return nullptr;
  }
}

void ravel_simulation_destroy(ravel_simulation* sim) {
  try {
    delete sim;
  } catch (...) {
    // Nothing useful to do, and nothing may cross the C boundary.
  }
}

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
