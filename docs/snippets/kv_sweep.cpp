// Sweeps the key-value store under many seeds.
//   kv_sweep [--bug none|ack-before-sync|trust-partial-tail] [ravel options...]
#include <iostream>
#include <string>
#include <vector>

#include "kv_store.hpp"
#include "ravel/sweep.hpp"

// [kv_main]
int main(int argc, char** argv) {
  kv::Bug bug = kv::Bug::None;

  // Take our own --bug flag out; everything else is ravel's command line.
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg != "--bug" || i + 1 >= argc) {
      args.push_back(arg);
      continue;
    }
    const std::string name = argv[++i];
    if (name == "ack-before-sync") bug = kv::Bug::AckBeforeSync;
    if (name == "trust-partial-tail") bug = kv::Bug::TrustPartialTail;
  }

  ravel::SweepDefaults defaults;
  defaults.seeds = 500;
  return ravel::run_sweep(std::cout, args, kv::setup(bug), defaults, "kv_sweep");
}
// [/kv_main]
