#pragma once

#include <array>
#include <cstdint>

namespace ravel {

// The one source of randomness a Simulation may use: xoshiro256**, seeded
// through splitmix64. Every operation is fixed-width integer arithmetic (or
// an exact conversion of it), so the same seed yields the same stream on
// every platform and compiler. std::mt19937 and the standard distributions
// are deliberately avoided; their output is not portable.
//
// NOT cryptographically secure. Never use it for keys, nonces or tokens.
class VirtualRng {
 public:
  explicit VirtualRng(std::uint64_t seed) noexcept;

  std::uint64_t next_u64() noexcept;

  // Uniform in [0, bound), without modulo bias. `bound` must be non-zero.
  std::uint64_t next_below(std::uint64_t bound) noexcept;

  // Uniform in [0, 1).
  double next_double() noexcept;

 private:
  std::array<std::uint64_t, 4> state_;
};

}  // namespace ravel
