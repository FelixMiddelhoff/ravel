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

void Trace::record(const TraceEvent& event) {
  events_.push_back(event);
  digest_ = mix(digest_, event.time);
  digest_ = mix(digest_, event.task);
  digest_ = mix(digest_, static_cast<std::uint64_t>(event.kind));
}

}  // namespace ravel
