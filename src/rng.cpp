#include "ravel/rng.hpp"

#include <utility>

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

VirtualRng VirtualRng::replaying(std::vector<Choice> choices) {
  VirtualRng rng(0);
  rng.replaying_ = true;
  rng.replay_ = std::move(choices);
  return rng;
}

std::uint64_t VirtualRng::next_raw() noexcept {
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

double VirtualRng::next_raw_double() noexcept {
  // The top 53 bits fit a double's mantissa exactly and scaling by 2^-53 is
  // exact too, so the result is identical everywhere.
  constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^-53
  return static_cast<double>(next_raw() >> 11) * kScale;
}

std::uint64_t VirtualRng::next_raw_below(std::uint64_t bound) noexcept {
  // Rejection sampling: drop the first `2^64 mod bound` values so the rest
  // divide evenly into `bound` buckets. (In unsigned arithmetic -bound is
  // 2^64 - bound, and 2^64 mod bound == (2^64 - bound) mod bound.)
  const std::uint64_t rejected_below = (0 - bound) % bound;
  std::uint64_t value = next_raw();
  while (value < rejected_below) value = next_raw();
  return value % bound;
}

VirtualRng::Choice VirtualRng::next_replayed(std::uint64_t bound) noexcept {
  Choice choice = replay_position_ < replay_.size() ? replay_[replay_position_] : 0;
  ++replay_position_;
  if (bound != 0 && choice >= bound) choice = bound - 1;
  return choice;
}

VirtualRng::Choice VirtualRng::record(Choice choice) {
  choices_.push_back(choice);
  return choice;
}

std::uint64_t VirtualRng::next_u64() {
  return record(replaying_ ? next_replayed(0) : next_raw());
}

std::uint64_t VirtualRng::next_below(std::uint64_t bound) {
  if (bound == 1) return 0;
  return record(replaying_ ? next_replayed(bound) : next_raw_below(bound));
}

bool VirtualRng::chance(double probability) {
  if (!(probability > 0.0)) return false;  // Also covers NaN.
  if (probability >= 1.0) return true;
  const Choice happens = replaying_ ? next_replayed(2) : (next_raw_double() < probability);
  return record(happens) != 0;
}

double VirtualRng::next_double() {
  constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^-53
  return static_cast<double>(next_u64() >> 11) * kScale;
}

}  // namespace ravel
