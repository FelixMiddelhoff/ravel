// The whole test program for CI: one line of ravel, one line of your setup.
#include "deposit.hpp"
#include "ravel/sweep.hpp"

// [ci_main]
int main(int argc, char** argv) {
  // The tutorial's deposit system, still with its bug.
  return ravel::run_sweep_main(argc, argv, deposit_setup(/*dedupe=*/false));
}
// [/ci_main]
