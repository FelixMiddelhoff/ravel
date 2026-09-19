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
  }
  return "Unknown";
}

bool is_task_event(TraceEventKind kind) noexcept {
  switch (kind) {
    case TraceEventKind::TaskSpawned:
    case TraceEventKind::TaskResumed:
    case TraceEventKind::TaskFinished:
    case TraceEventKind::TaskThrew:
      return true;
    case TraceEventKind::MessageSent:
    case TraceEventKind::MessageDropped:
    case TraceEventKind::MessageDelivered:
      return false;
  }
  return false;
}

void Trace::record(const TraceEvent& event) {
  events_.push_back(event);
  digest_ = mix(digest_, event.time);
  digest_ = mix(digest_, event.subject);
  digest_ = mix(digest_, static_cast<std::uint64_t>(event.kind));
}

}  // namespace ravel
