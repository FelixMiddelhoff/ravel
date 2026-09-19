// The quorum-register example doubles as an end-to-end test: ravel must find
// the bug in the buggy design, leave the corrected one alone, and shrink and
// replay the failure.
#include "quorum_register.hpp"
#include "ravel/runner.hpp"
#include "testing.hpp"

namespace {

ravel::RunnerOptions sweep(bool shrink) {
  ravel::RunnerOptions options;
  options.seed_count = 500;
  options.shrink_first_failure = shrink;
  return options;
}

}  // namespace

TEST(quorum_register_with_write_quorum_one_loses_acknowledged_writes) {
  const ravel::RunnerReport report = ravel::run_seeds(quorum_register::setup(1), sweep(false));
  CHECK(!report.ok());
  CHECK(report.failures.front().failure == "invariant 'read_sees_acknowledged_write' failed");
  CHECK(report.failures.size() < 500);  // Most seeds get lucky; some do not.
}

TEST(quorum_register_with_overlapping_quorums_never_fails) {
  CHECK(ravel::run_seeds(quorum_register::setup(2), sweep(false)).ok());
  CHECK(ravel::run_seeds(quorum_register::setup(3), sweep(false)).ok());
}

TEST(quorum_register_bug_shrinks_and_replays) {
  const ravel::RunnerReport report = ravel::run_seeds(quorum_register::setup(1), sweep(true));
  CHECK(report.shrunk.has_value());
  if (!report.shrunk) return;

  const ravel::ShrinkResult& shrunk = *report.shrunk;
  CHECK(!shrunk.minimal.ok);
  CHECK(shrunk.minimal.failure == shrunk.original.failure);
  CHECK(shrunk.choices.size() < shrunk.original_choices.size());

  const ravel::Result replayed = ravel::replay(quorum_register::setup(1), shrunk.choices);
  CHECK(!replayed.ok);
  CHECK(replayed.trace_digest == shrunk.minimal.trace_digest);
}
