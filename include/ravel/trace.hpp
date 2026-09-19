#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ravel/clock.hpp"

namespace ravel {

enum class TraceEventKind : std::uint8_t {
  TaskSpawned,
  TaskResumed,
  TaskFinished,
  TaskThrew,
};

// One scheduling fact. The event's position in the trace is its step number.
struct TraceEvent {
  VirtualClock::Tick time;
  std::size_t task;  // Scheduler-assigned task id.
  TraceEventKind kind;
};

// Ordered record of everything the Scheduler decided during a run, plus a
// running digest of it. Two runs made the same decisions exactly when their
// digests match, which is how replay is checked.
class Trace {
 public:
  void record(const TraceEvent& event);

  const std::vector<TraceEvent>& events() const noexcept { return events_; }
  std::uint64_t digest() const noexcept { return digest_; }

 private:
  std::vector<TraceEvent> events_;
  std::uint64_t digest_ = 0xCBF29CE484222325ULL;  // FNV-1a offset basis
};

}  // namespace ravel
