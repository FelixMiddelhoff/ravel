#pragma once

#include <cstdint>

namespace ravel {

class Scheduler;

/// Monotonic virtual time. It never touches the wall clock, so a run is
/// reproducible regardless of host speed or load, and only the Scheduler can
/// move it forward.
class VirtualClock {
 public:
  /// Virtual time, in ticks. A tick has no fixed real-world length.
  using Tick = std::uint64_t;

  /// The current virtual time.
  Tick now() const noexcept { return now_; }

 private:
  friend class Scheduler;

  void advance_to(Tick time) noexcept { now_ = time; }

  Tick now_ = 0;
};

}  // namespace ravel
