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
  MessageSent,
  MessageDropped,
  MessageDelivered,
  DiskWritten,
  DiskSynced,
  DiskFailed,   ///< A write or sync that returned an error.
  DiskCrashed,
};

const char* to_string(TraceEventKind kind) noexcept;

/// What an event's `subject` id refers to.
enum class TraceSubject : std::uint8_t { Task, Channel, Disk };
TraceSubject subject_of(TraceEventKind kind) noexcept;

/// One fact about the run. The event's position in the trace is its step
/// number.
struct TraceEvent {
  /// When it happened, in virtual ticks.
  VirtualClock::Tick time;
  std::size_t subject;  ///< A task, channel or disk id; see subject_of().
  /// What happened.
  TraceEventKind kind;

  /// Events are equal when all their fields are.
  bool operator==(const TraceEvent&) const = default;
};

/// Ordered record of everything that happened during a run (scheduling
/// decisions and message fates), plus a running digest of it. Two runs made
/// the same decisions exactly when their digests match, which is how replay
/// is checked.
class Trace {
 public:
  /// Appends an event and folds it into the digest.
  void record(const TraceEvent& event);

  /// Every event so far, in order.
  const std::vector<TraceEvent>& events() const noexcept { return events_; }
  /// A fingerprint of the events so far; equal digests mean identical runs.
  std::uint64_t digest() const noexcept { return digest_; }

 private:
  std::vector<TraceEvent> events_;
  std::uint64_t digest_ = 0xCBF29CE484222325ULL;  ///< FNV-1a offset basis
};

}  // namespace ravel
