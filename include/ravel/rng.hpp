#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ravel {

// The one source of randomness a Simulation may use. Every draw is a
// "choice": a bounded integer, where 0 is always the simplest outcome (the
// first option, no fault, the shortest delay). The generator records every
// choice it makes, and can later replay a recorded list instead of drawing
// fresh numbers. That is what lets a failing run be shrunk: edit the list,
// replay it, and see whether the bug survives.
//
// Generating mode (the default): xoshiro256** seeded through splitmix64.
// Every operation is fixed-width integer arithmetic (or an exact conversion
// of it), so the same seed yields the same choices on every platform and
// compiler. std::mt19937 and the standard distributions are deliberately
// avoided; their output is not portable.
//
// Replaying mode: choices come from the list. Each is clamped to the bound of
// the draw it answers, so any list is valid, and draws past the end of the
// list get 0.
//
// NOT cryptographically secure. Never use it for keys, nonces or tokens.
class VirtualRng {
 public:
  using Choice = std::uint64_t;

  explicit VirtualRng(std::uint64_t seed) noexcept;

  // A generator that answers draws from `choices` instead of computing them.
  static VirtualRng replaying(std::vector<Choice> choices);

  // Uniform over all 64-bit values.
  std::uint64_t next_u64();

  // Uniform in [0, bound), without modulo bias. `bound` must be non-zero.
  // A bound of 1 has only one outcome, so it neither draws nor records.
  std::uint64_t next_below(std::uint64_t bound);

  // Uniform in [low, high], both included. `low` must not exceed `high`.
  std::uint64_t next_between(std::uint64_t low, std::uint64_t high);

  // True with the given probability. Recorded as 1 (true) or 0 (false), so
  // shrinking toward 0 removes the event. Probabilities of 0 or less, and 1
  // or more, are certain and neither draw nor record.
  bool chance(double probability);

  // Uniform in [0, 1). Built from next_u64(), so it shrinks toward 0.0.
  double next_double();

  // Every choice made so far, in order: what was actually used, after
  // clamping. Replaying this list reproduces the run exactly.
  const std::vector<Choice>& choices() const noexcept { return choices_; }

 private:
  std::uint64_t next_raw() noexcept;  // One xoshiro256** step.
  double next_raw_double() noexcept;  // In [0, 1), from one raw step.
  std::uint64_t next_raw_below(std::uint64_t bound) noexcept;

  // The next list entry, clamped to `bound` (0 means unbounded).
  Choice next_replayed(std::uint64_t bound) noexcept;

  Choice record(Choice choice);

  std::array<std::uint64_t, 4> state_{};
  std::vector<Choice> choices_;

  bool replaying_ = false;
  std::vector<Choice> replay_;
  std::size_t replay_position_ = 0;
};

}  // namespace ravel
