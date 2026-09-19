#include "ravel/trace.hpp"

namespace ravel {

namespace {

// FNV-1a, fed one byte at a time in a fixed (little-endian) order so the
// digest does not depend on the host's endianness.
std::uint64_t mix(std::uint64_t digest, std::uint64_t value) noexcept {
  constexpr std::uint64_t kPrime = 0x100000001B3ULL;
  for (int byte = 0; byte < 8; ++byte) {
    digest ^= (value >> (8 * byte)) & 0xFF;
    digest *= kPrime;
  }
  return digest;
}

}  // namespace

const char* to_string(TraceEventKind kind) noexcept {
  switch (kind) {
    case TraceEventKind::TaskSpawned: return "TaskSpawned";
    case TraceEventKind::TaskResumed: return "TaskResumed";
    case TraceEventKind::TaskFinished: return "TaskFinished";
    case TraceEventKind::TaskThrew: return "TaskThrew";
    case TraceEventKind::MessageSent: return "MessageSent";
    case TraceEventKind::MessageDropped: return "MessageDropped";
    case TraceEventKind::MessageDelivered: return "MessageDelivered";
    case TraceEventKind::DiskWritten: return "DiskWritten";
    case TraceEventKind::DiskSynced: return "DiskSynced";
    case TraceEventKind::DiskFailed: return "DiskFailed";
    case TraceEventKind::DiskCrashed: return "DiskCrashed";
  }
  return "Unknown";
}

TraceSubject subject_of(TraceEventKind kind) noexcept {
  switch (kind) {
    case TraceEventKind::TaskSpawned:
    case TraceEventKind::TaskResumed:
    case TraceEventKind::TaskFinished:
    case TraceEventKind::TaskThrew:
      return TraceSubject::Task;
    case TraceEventKind::MessageSent:
    case TraceEventKind::MessageDropped:
    case TraceEventKind::MessageDelivered:
      return TraceSubject::Channel;
    case TraceEventKind::DiskWritten:
    case TraceEventKind::DiskSynced:
    case TraceEventKind::DiskFailed:
    case TraceEventKind::DiskCrashed:
      return TraceSubject::Disk;
  }
  return TraceSubject::Task;
}

void Trace::record(const TraceEvent& event) {
  events_.push_back(event);
  digest_ = mix(digest_, event.time);
  digest_ = mix(digest_, event.subject);
  digest_ = mix(digest_, static_cast<std::uint64_t>(event.kind));
}

}  // namespace ravel
