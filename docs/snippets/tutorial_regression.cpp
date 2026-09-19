// A regression test made from a saved reproducer.
#include <cstdio>

#include "deposit.hpp"
#include "ravel/shrink.hpp"

// [regression]
// Found by ravel, shrunk to this, and checked in. No seed needed.
const ravel::Choices kDoubleDeposit = {0, 0, 0, 1};

int main() {
  const ravel::Result fixed = ravel::replay(deposit_setup(/*dedupe=*/true), kDoubleDeposit);
  const ravel::Result buggy = ravel::replay(deposit_setup(/*dedupe=*/false), kDoubleDeposit);

  std::printf("fixed server: %s\n", fixed.ok ? "passes" : "FAILS");
  std::printf("buggy server: %s\n", buggy.ok ? "passes" : ("fails: " + buggy.failure).c_str());
  return fixed.ok && !buggy.ok ? 0 : 1;
}
// [/regression]
