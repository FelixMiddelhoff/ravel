// Raft on ravel, as an end-to-end test of the whole library: a correct
// implementation must survive every seed, and each deliberate bug must be found.
//
// The bug tests look at a small window of seeds known to contain a failure
// (runs are deterministic, so the window is stable), because two of the bugs
// show up on only a few seeds in a hundred and a full sweep would make the test
// suite slow. If a change to ravel alters what seeds do, these windows move:
// find the new first failing seed with `ravel_raft` and update them.
#include <cstdint>
#include <memory>
#include <string>

#include "raft.hpp"
#include "ravel/runner.hpp"
#include "testing.hpp"

namespace {

ravel::RunnerReport sweep(raft::Bug bug, std::uint64_t first_seed, std::uint64_t count,
                          bool shrink = false) {
  raft::Config config;
  config.bug = bug;

  ravel::RunnerOptions options;
  options.first_seed = first_seed;
  options.seed_count = count;
  options.shrink_first_failure = shrink;
  options.max_shrink_attempts = 300;  // Raft runs are long; this is plenty here.
  options.simulation = raft::options(config);
  return ravel::run_seeds(raft::setup(config), options);
}

}  // namespace

TEST(raft_elects_a_leader_and_commits_entries) {
  raft::Config config;
  auto history = std::make_shared<raft::History>();
  ravel::Simulation sim(1, raft::options(config));
  raft::setup(config, history)(sim);

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(result.ok);
  CHECK(history->leaders_elected >= 1);
  CHECK(history->entries_applied >= 5);  // Progress, not just safety.
}

TEST(raft_survives_crashes_and_message_loss_on_every_seed) {
  const ravel::RunnerReport report = sweep(raft::Bug::None, 0, 200);
  CHECK(report.ok());
  if (!report.ok()) {
    std::printf("      seed %llu: %s\n", static_cast<unsigned long long>(report.failures[0].seed),
                report.failures[0].failure.c_str());
  }
}

TEST(raft_without_file_sync_loses_acknowledged_state) {
  CHECK(!sweep(raft::Bug::SkipFileSync, 0, 40).ok());
}

TEST(raft_without_directory_sync_loses_acknowledged_state) {
  // Rare: about one seed in 600. See raft.hpp for why.
  CHECK(!sweep(raft::Bug::SkipDirectorySync, 400, 60).ok());
}

TEST(raft_ignoring_log_freshness_loses_committed_entries) {
  CHECK(!sweep(raft::Bug::IgnoreLogUpToDateCheck, 40, 60).ok());
}

TEST(a_raft_bug_shrinks_and_replays) {
  const ravel::RunnerReport report = sweep(raft::Bug::SkipFileSync, 0, 40, /*shrink=*/true);
  CHECK(report.shrunk.has_value());
  if (!report.shrunk) return;

  const ravel::ShrinkResult& shrunk = *report.shrunk;
  CHECK(!shrunk.minimal.ok);
  CHECK(shrunk.minimal.failure == shrunk.original.failure);
  CHECK(shrunk.choices.size() < shrunk.original_choices.size());

  raft::Config config;
  config.bug = raft::Bug::SkipFileSync;
  const ravel::Result replayed =
      ravel::replay(raft::setup(config), shrunk.choices, raft::options(config));
  CHECK(!replayed.ok);
  CHECK(replayed.trace_digest == shrunk.minimal.trace_digest);
}
