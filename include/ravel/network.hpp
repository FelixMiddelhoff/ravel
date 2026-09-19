#pragma once

#include <string>
#include <utility>

#include "ravel/clock.hpp"

namespace ravel {

// Injectable fault behavior for a Channel. Every decision (drop? reorder?
// how much latency?) will be drawn from the Simulation's VirtualRng, so it
// replays identically for a given seed.
struct FaultSpec {
  double loss_probability = 0.0;  // In [0, 1).
  VirtualClock::Tick latency_min = 0;
  VirtualClock::Tick latency_max = 0;
  bool allow_reorder = false;
};

// A virtual, in-process channel between two named endpoints. There are no
// real sockets: delivery will be driven entirely by the Scheduler advancing
// virtual time, so a run never depends on actual network conditions.
//
// Not wired to the Scheduler yet; a Channel currently only records its
// endpoints and fault settings.
class Channel {
 public:
  Channel(std::string from, std::string to, FaultSpec fault)
      : from_(std::move(from)), to_(std::move(to)), fault_(fault) {}

  const std::string& from() const noexcept { return from_; }
  const std::string& to() const noexcept { return to_; }
  const FaultSpec& fault() const noexcept { return fault_; }

 private:
  std::string from_;
  std::string to_;
  FaultSpec fault_;
};

}  // namespace ravel
