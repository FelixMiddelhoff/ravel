// Everything in ravel rests on one property: the same seed produces the same
// stream, on every platform. These tests pin it down.
#include <cstdint>

#include "ravel/rng.hpp"
#include "testing.hpp"

TEST(rng_same_seed_gives_same_stream) {
  ravel::VirtualRng a(42);
  ravel::VirtualRng b(42);
  for (int i = 0; i < 1000; ++i) CHECK(a.next_u64() == b.next_u64());
}

TEST(rng_different_seeds_diverge) {
  ravel::VirtualRng a(42);
  ravel::VirtualRng b(43);
  bool diverged = false;
  for (int i = 0; i < 8; ++i) diverged |= a.next_u64() != b.next_u64();
  CHECK(diverged);
}

// Golden values (cross-checked against an independent implementation). If
// these change, every saved seed changes meaning; if CI on another platform
// disagrees, the RNG is not portable.
TEST(rng_stream_matches_golden_values) {
  ravel::VirtualRng rng(42);
  CHECK(rng.next_u64() == 0x15780B2E0C2EC716ULL);
  CHECK(rng.next_u64() == 0x6104D9866D113A7EULL);
  CHECK(rng.next_u64() == 0xAE17533239E499A1ULL);
  CHECK(rng.next_u64() == 0xECB8AD4703B360A1ULL);
}

TEST(rng_next_below_stays_in_bounds_and_covers_the_range) {
  ravel::VirtualRng rng(7);
  constexpr std::uint64_t kBound = 5;
  bool seen[kBound] = {};
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t value = rng.next_below(kBound);
    CHECK(value < kBound);
    if (value < kBound) seen[value] = true;
  }
  for (bool was_seen : seen) CHECK(was_seen);
}

TEST(rng_next_below_one_is_always_zero) {
  ravel::VirtualRng rng(7);
  for (int i = 0; i < 100; ++i) CHECK(rng.next_below(1) == 0);
}

TEST(rng_next_double_is_in_unit_interval) {
  ravel::VirtualRng rng(7);
  for (int i = 0; i < 1000; ++i) {
    const double value = rng.next_double();
    CHECK(value >= 0.0 && value < 1.0);
  }
}
