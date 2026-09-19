#include "ravel/simulation.hpp"

namespace ravel {

Simulation::Simulation(std::uint64_t seed, SimulationOptions options)
    : seed_(seed), options_(options), rng_(seed), scheduler_(clock_, rng_, trace_) {}

Channel& Simulation::add_channel(std::string from, std::string to, FaultSpec fault) {
  return channels_.emplace_back(channels_.size(), std::move(from), std::move(to), fault,
                                scheduler_, rng_, trace_);
}

void Simulation::add_invariant(std::string name, InvariantFn invariant) {
  invariants_.push_back({std::move(name), std::move(invariant)});
}

std::string Simulation::first_failed_invariant() const {
  for (const NamedInvariant& invariant : invariants_) {
    try {
      if (!invariant.check()) return "invariant '" + invariant.name + "' failed";
    } catch (...) {
      return "invariant '" + invariant.name + "' threw";
    }
  }
  return {};
}

Result Simulation::run_until_quiescent() {
  const RunReport report = scheduler_.run_until_quiescent(options_.max_steps);

  Result result;
  result.seed = seed_;
  result.steps = report.steps;
  result.trace_digest = trace_.digest();
  result.failure =
      report.status == RunStatus::Completed ? first_failed_invariant() : report.failure;
  result.ok = result.failure.empty();
  return result;
}

}  // namespace ravel
