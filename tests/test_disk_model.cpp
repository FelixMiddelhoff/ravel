// Differential test: random operation sequences run on ravel::Disk and on the
// reference model in disk_model.hpp must agree on every result and on the
// durable state after every step.
#include <cstdint>
#include <string>

#include "disk_model.hpp"
#include "testing.hpp"

TEST(disk_agrees_with_the_reference_model_on_random_sequences) {
  int reported = 0;
  for (std::uint64_t seed = 0; seed < 3000; ++seed) {
    const std::string difference = ravel::testing::differential_disk_test(seed, 40);
    if (!difference.empty() && reported++ < 3) std::fprintf(stderr, "%s\n", difference.c_str());
    CHECK(difference.empty());
  }
}
