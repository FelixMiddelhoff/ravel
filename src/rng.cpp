#include "ravel/rng.hpp"

namespace ravel {

namespace {

// Spreads one 64-bit seed into the four words of xoshiro256** state.
std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

std::uint64_t rotate_left(std::uint64_t value, int bits) noexcept {
  return (value << bits) | (value >> (64 - bits));
}

}  // namespace

VirtualRng::VirtualRng(std::uint64_t seed) noexcept {
  std::uint64_t splitmix_state = seed;
  for (auto& word : state_) word = splitmix64(splitmix_state);
}

std::uint64_t VirtualRng::next_u64() noexcept {
  const std::uint64_t result = rotate_left(state_[1] * 5, 7) * 9;
  const std::uint64_t shifted = state_[1] << 17;

  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= shifted;
  state_[3] = rotate_left(state_[3], 45);

  return result;
}

std::uint64_t VirtualRng::next_below(std::uint64_t bound) noexcept {
  // Rejection sampling: drop the first `2^64 mod bound` values so the rest
  // divide evenly into `bound` buckets. (In unsigned arithmetic -bound is
  // 2^64 - bound, and 2^64 mod bound == (2^64 - bound) mod bound.)
  const std::uint64_t rejected_below = (0 - bound) % bound;
  std::uint64_t value = next_u64();
  while (value < rejected_below) value = next_u64();
  return value % bound;
}

double VirtualRng::next_double() noexcept {
  // The top 53 bits fit a double's mantissa exactly and scaling by 2^-53 is
  // exact too, so the result is identical everywhere.
  constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^-53
  return static_cast<double>(next_u64() >> 11) * kScale;
}

}  // namespace ravel
