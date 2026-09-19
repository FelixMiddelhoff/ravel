// Finds a real distributed-systems bug and boils it down.
//
// A three-replica register acknowledges a write after one replica has it,
// then reads from two. Nothing forces those two to include the replica that
// has the write. ravel runs 1000 seeds of the buggy design, finds the seeds
// where the read is stale, and shrinks one to its minimum. The corrected
// design (wait for two acknowledgements) survives all 1000.
#include <cstdio>

#include "quorum_register.hpp"
#include "ravel/runner.hpp"

namespace {

void check(const char* label, int write_quorum, bool shrink) {
  ravel::RunnerOptions options;
  options.seed_count = 1000;
  options.shrink_first_failure = shrink;
  options.simulation.trace_dir = "ravel-traces";

  const ravel::RunnerReport report =
      ravel::run_seeds(quorum_register::setup(write_quorum), options);

  std::printf("%s (write quorum %d): %zu of %llu seeds fail\n", label, write_quorum,
              report.failures.size(), static_cast<unsigned long long>(report.seeds_run));
  if (report.ok()) return;

  const ravel::Result& first = report.failures.front();
  std::printf("  first failing seed %llu: %s\n", static_cast<unsigned long long>(first.seed),
              first.failure.c_str());

  if (report.shrunk) {
    const ravel::ShrinkResult& shrunk = *report.shrunk;
    std::printf("  shrunk from %zu random choices (%llu steps) to %zu (%llu steps)\n",
                shrunk.original_choices.size(), static_cast<unsigned long long>(shrunk.original.steps),
                shrunk.choices.size(), static_cast<unsigned long long>(shrunk.minimal.steps));
    std::printf("  replayable reproducer: %s\n  trace of the minimal run: %s\n",
                shrunk.choices_path.c_str(), shrunk.minimal.trace_path.c_str());
  }
}

}  // namespace

int main() {
  check("buggy", /*write_quorum=*/1, /*shrink=*/true);
  check("fixed", /*write_quorum=*/2, /*shrink=*/false);
  return 0;
}
