// Everything in ravel rests on one property: the same seed produces the same
// stream, on every platform. These tests pin it down.
#include <cstdint>
#include <vector>

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

TEST(rng_records_every_choice_it_makes) {
  ravel::VirtualRng rng(9);
  const std::uint64_t below = rng.next_below(10);
  const bool happened = rng.chance(0.5);
  const std::uint64_t raw = rng.next_u64();

  const std::vector<std::uint64_t> expected = {below, happened ? 1u : 0u, raw};
  CHECK(rng.choices() == expected);
}

TEST(rng_replays_a_recorded_list_exactly) {
  ravel::VirtualRng original(9);
  std::vector<std::uint64_t> drawn;
  for (int i = 0; i < 50; ++i) drawn.push_back(original.next_below(7));
  for (int i = 0; i < 50; ++i) drawn.push_back(original.chance(0.3) ? 1 : 0);
  drawn.push_back(original.next_u64());

  ravel::VirtualRng replay = ravel::VirtualRng::replaying(original.choices());
  std::vector<std::uint64_t> replayed;
  for (int i = 0; i < 50; ++i) replayed.push_back(replay.next_below(7));
  for (int i = 0; i < 50; ++i) replayed.push_back(replay.chance(0.3) ? 1 : 0);
  replayed.push_back(replay.next_u64());

  CHECK(replayed == drawn);
  CHECK(replay.choices() == original.choices());
}

TEST(rng_replay_clamps_to_the_bound_and_defaults_to_zero_past_the_end) {
  ravel::VirtualRng rng = ravel::VirtualRng::replaying({100, 3});
  CHECK(rng.next_below(10) == 9);  // 100 clamped to bound - 1.
  CHECK(rng.chance(0.5));          // 3 clamped to 1.
  CHECK(rng.next_below(10) == 0);  // Past the end.
  CHECK(!rng.chance(0.5));
  CHECK((rng.choices() == std::vector<std::uint64_t>{9, 1, 0, 0}));  // What was used.
}

TEST(rng_certain_draws_consume_and_record_nothing) {
  ravel::VirtualRng rng(9);
  CHECK(rng.next_below(1) == 0);
  CHECK(!rng.chance(0.0));
  CHECK(!rng.chance(-1.0));
  CHECK(rng.chance(1.0));
  CHECK(rng.chance(2.0));
  CHECK(rng.choices().empty());
}

TEST(rng_next_below_handles_bounds_that_reject_many_raw_values) {
  // Just over 2^63: about half of all raw values fall in the rejected zone.
  constexpr std::uint64_t kBound = (std::uint64_t{1} << 63) + 1;
  ravel::VirtualRng rng(3);
  for (int i = 0; i < 200; ++i) CHECK(rng.next_below(kBound) < kBound);
}

TEST(rng_next_between_covers_the_whole_64_bit_range) {
  ravel::VirtualRng rng(3);
  rng.next_between(0, UINT64_MAX);  // span + 1 would overflow; must not crash or loop.
  CHECK(rng.next_between(5, 5) == 5);
  CHECK(rng.choices().size() >= 1);
}
