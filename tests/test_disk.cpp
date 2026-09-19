#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"
#include "testing.hpp"

namespace {

// Runs `body` as the only task of a fresh simulation with one disk.
template <typename Body>
ravel::Result run_with_disk(ravel::Simulation& sim, ravel::Disk& disk, Body body) {
  (void)disk;
  sim.scheduler().spawn("test", std::move(body));
  return sim.run_until_quiescent();
}

}  // namespace

TEST(disk_read_sees_a_write_before_it_is_synced) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  std::string read_back;
  run_with_disk(sim, disk, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "hello");
    read_back = (co_await disk.read("f", 0, 100)).data;
  });
  CHECK(read_back == "hello");
  CHECK(disk.durable_contents("f").empty());  // Not synced yet.
  CHECK(disk.file_size("f") == 5);
}

TEST(disk_sync_makes_writes_durable) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  run_with_disk(sim, disk, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "hello");
    co_await disk.sync("f");
  });
  CHECK(disk.durable_contents("f") == "hello");
}

TEST(disk_writes_at_an_offset_and_zero_fills_the_gap) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  std::string read_back;
  run_with_disk(sim, disk, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "ab");
    co_await disk.write("f", 4, "cd");
    co_await disk.write("f", 1, "X");  // Overwrites the 'b'.
    read_back = (co_await disk.read("f", 0, 100)).data;
  });
  CHECK((read_back == std::string("aX\0\0cd", 6)));
}

TEST(disk_reads_are_cut_at_the_end_of_the_file) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  std::string middle;
  std::string past_end;
  ravel::DiskStatus missing_status = ravel::DiskStatus::Ok;
  std::string missing;
  run_with_disk(sim, disk, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "0123456789");
    middle = (co_await disk.read("f", 8, 100)).data;
    past_end = (co_await disk.read("f", 10, 5)).data;
    const ravel::ReadResult none = co_await disk.read("nope", 0, 5);
    missing_status = none.status;
    missing = none.data;
  });
  CHECK(middle == "89");
  CHECK(past_end.empty());
  CHECK(missing_status == ravel::DiskStatus::NotFound);
  CHECK(missing.empty());
}

TEST(disk_operations_take_virtual_time) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d", {.latency_min = 5, .latency_max = 5});
  ravel::VirtualClock::Tick after_write = 0;
  ravel::VirtualClock::Tick after_sync = 0;
  run_with_disk(sim, disk, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "x");
    after_write = sim.clock().now();
    co_await disk.sync("f");
    after_sync = sim.clock().now();
  });
  CHECK(after_write == 5);
  CHECK(after_sync == 10);
}

TEST(disk_refuses_writes_beyond_its_capacity) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d", {.capacity_bytes = 10});
  ravel::DiskStatus first = ravel::DiskStatus::IoError;
  ravel::DiskStatus too_big = ravel::DiskStatus::Ok;
  ravel::DiskStatus overwrite = ravel::DiskStatus::IoError;
  run_with_disk(sim, disk, [&]() -> ravel::Task {
    first = co_await disk.write("a", 0, "12345678");        // 8 of 10 used.
    too_big = co_await disk.write("b", 0, "12345");          // Would make 13.
    overwrite = co_await disk.write("a", 0, "abcdefgh");     // Same size: fits.
  });
  CHECK(first == ravel::DiskStatus::Ok);
  CHECK(too_big == ravel::DiskStatus::NoSpace);
  CHECK(overwrite == ravel::DiskStatus::Ok);
  CHECK(disk.file_size("b") == 0);  // A refused write leaves no trace.
  CHECK(disk.used_bytes() == 8);
}

TEST(disk_injects_write_errors) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d", {.write_error_probability = 1.0});
  ravel::DiskStatus status = ravel::DiskStatus::Ok;
  run_with_disk(sim, disk, [&]() -> ravel::Task { status = co_await disk.write("f", 0, "x"); });
  CHECK(status == ravel::DiskStatus::IoError);
  CHECK(disk.file_size("f") == 0);
}

TEST(disk_failed_sync_drops_the_data_it_should_have_flushed) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d", {.sync_error_probability = 1.0});
  ravel::DiskStatus status = ravel::DiskStatus::Ok;
  std::string read_back = "unset";
  run_with_disk(sim, disk, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "precious");
    status = co_await disk.sync("f");
    read_back = (co_await disk.read("f", 0, 100)).data;
  });
  CHECK(status == ravel::DiskStatus::IoError);
  CHECK(read_back.empty());  // Gone from the cache too, not just from disk.
  CHECK(disk.durable_contents("f").empty());
}

TEST(disk_sync_only_covers_writes_that_completed_before_it_started) {
  // Latency 10 everywhere. w1 completes at t=10, before the sync is issued at
  // t=11. w2 is issued at t=8, so it completes at t=18, while the sync is in
  // flight (until t=21): it must not count as synced.
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d", {.latency_min = 10, .latency_max = 10});
  sim.scheduler().spawn("w1", [&]() -> ravel::Task { co_await disk.write("f", 0, "1"); });
  sim.scheduler().spawn("w2", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(8);
    co_await disk.write("f", 1, "2");
  });
  sim.scheduler().spawn("syncer", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(11);
    co_await disk.sync("f");
  });
  sim.run_until_quiescent();

  CHECK(disk.durable_contents("f") == "1");
  CHECK(disk.file_size("f") == 2);  // w2 is visible, just not durable.
}

TEST(disk_crash_never_loses_synced_data) {
  for (std::uint64_t seed = 0; seed < 100; ++seed) {
    ravel::Simulation sim(seed);
    ravel::Disk& disk = sim.add_disk("d");
    run_with_disk(sim, disk, [&]() -> ravel::Task {
      co_await disk.write("f", 0, std::string(1500, 'a'));
      co_await disk.sync("f");
      co_await disk.write("f", 0, std::string(1500, 'b'));  // Unsynced overwrite.
      disk.crash();
    });
    const std::string durable = disk.durable_contents("f");
    CHECK(durable.size() == 1500);
    // Each byte is from the synced 'a' or from a surviving part of the 'b's.
    CHECK(durable.find_first_not_of("ab") == std::string::npos);
    CHECK(durable[0] == 'a' || durable[0] == 'b');
  }
}

TEST(disk_crash_can_lose_tear_or_keep_an_unsynced_write) {
  std::set<std::size_t> durable_sizes;
  for (std::uint64_t seed = 0; seed < 60; ++seed) {
    ravel::Simulation sim(seed);
    ravel::Disk& disk = sim.add_disk("d");
    run_with_disk(sim, disk, [&]() -> ravel::Task {
      co_await disk.write("f", 0, std::string(2000, 'x'));  // Four sectors, unsynced.
      disk.crash();
    });
    durable_sizes.insert(disk.durable_contents("f").size());
  }
  // Lost (0), torn at 512/1024/1536, or whole (2000).
  for (const std::size_t size : durable_sizes) {
    CHECK(size == 0 || size == 512 || size == 1024 || size == 1536 || size == 2000);
  }
  CHECK(durable_sizes.count(0) == 1);
  CHECK(durable_sizes.count(2000) == 1);
  CHECK(durable_sizes.size() > 2);  // At least one torn outcome too.
}

TEST(disk_crash_outcomes_follow_the_recorded_choices) {
  const auto crash_with = [](ravel::Choices choices) {
    ravel::Simulation sim(0, ravel::SimulationOptions{.replay_choices = std::move(choices)});
    ravel::Disk& disk = sim.add_disk("d");
    run_with_disk(sim, disk, [&]() -> ravel::Task {
      co_await disk.write("f", 0, std::string(1300, 'x'));  // Three sectors.
      disk.crash();
    });
    return disk.durable_contents("f").size();
  };
  // The first choice is whether the file's creation survives (1), then the
  // fate of its one unsynced write.
  CHECK(crash_with({0}) == 0);           // The file itself is gone.
  CHECK(crash_with({1, 0}) == 0);        // Write lost.
  CHECK(crash_with({1, 2}) == 1300);     // Whole.
  CHECK(crash_with({1, 1, 1}) == 512);   // Torn after one sector.
  CHECK(crash_with({1, 1, 2}) == 1024);  // Torn after two.
  CHECK(crash_with({1, 1, 0}) == 0);     // Torn before the first sector: lost.
}

TEST(disk_crash_fails_operations_still_in_flight) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d", {.latency_min = 10, .latency_max = 10});
  ravel::DiskStatus status = ravel::DiskStatus::Ok;
  sim.scheduler().spawn("writer", [&]() -> ravel::Task {
    status = co_await disk.write("f", 0, "x");  // Would complete at t=10.
  });
  sim.scheduler().spawn("power_cut", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(5);
    disk.crash();
  });
  sim.run_until_quiescent();

  CHECK(status == ravel::DiskStatus::Crashed);
  CHECK(disk.file_size("f") == 0);
}

TEST(disk_faults_replay_exactly_for_a_seed) {
  const auto run = [](std::uint64_t seed) {
    ravel::Simulation sim(seed);
    ravel::Disk& disk = sim.add_disk("d", {.latency_min = 1,
                                           .latency_max = 9,
                                           .write_error_probability = 0.2,
                                           .sync_error_probability = 0.2});
    sim.scheduler().spawn("worker", [&]() -> ravel::Task {
      for (int i = 0; i < 20; ++i) {
        co_await disk.write("f", static_cast<std::uint64_t>(i) * 700, std::string(700, 'x'));
        if (i % 3 == 0) co_await disk.sync("f");
      }
      disk.crash();
    });
    return sim.run_until_quiescent().trace_digest;
  };
  for (std::uint64_t seed = 0; seed < 20; ++seed) CHECK(run(seed) == run(seed));
  CHECK(run(1) != run(2));
}

TEST(disk_rejects_an_invalid_fault_spec) {
  ravel::Simulation sim(1);
  bool probability_rejected = false;
  bool latency_rejected = false;
  try {
    sim.add_disk("d", {.write_error_probability = 2.0});
  } catch (const std::invalid_argument&) {
    probability_rejected = true;
  }
  try {
    sim.add_disk("d", {.latency_min = 5, .latency_max = 1});
  } catch (const std::invalid_argument&) {
    latency_rejected = true;
  }
  CHECK(probability_rejected);
  CHECK(latency_rejected);
}

// The classic: acknowledge a write, forget to sync, lose power. ravel finds it
// on every seed where the crash drops the record, and shrinks it to nothing.
namespace {

ravel::SimulationSetup write_ahead_log_setup(bool sync_before_ack) {
  return [sync_before_ack](ravel::Simulation& sim) {
    ravel::Disk& disk = sim.add_disk("ssd", {.latency_min = 1, .latency_max = 10});
    bool& acknowledged = sim.make_state<bool>(false);

    sim.scheduler().spawn("database", [&, sync_before_ack]() -> ravel::Task {
      co_await disk.write("wal", 0, "commit #1");
      if (sync_before_ack) co_await disk.sync("wal");
      acknowledged = true;  // The client is told the commit is safe.
    });
    sim.scheduler().spawn("power_cut", [&]() -> ravel::Task {
      co_await sim.scheduler().sleep(1000);
      disk.crash();
    });
    sim.add_invariant("acknowledged_commits_survive_a_crash", [&acknowledged, &disk] {
      return !acknowledged || disk.durable_contents("wal") == "commit #1";
    });
  };
}

}  // namespace

TEST(disk_finds_the_missing_fsync_before_acknowledging) {
  ravel::RunnerOptions options;
  options.seed_count = 200;
  options.shrink_first_failure = true;

  const ravel::RunnerReport buggy = ravel::run_seeds(write_ahead_log_setup(false), options);
  CHECK(!buggy.ok());
  CHECK(buggy.failures.size() < 200);  // Some crashes happen to keep the record.
  CHECK(buggy.shrunk.has_value());
  if (buggy.shrunk) {
    CHECK(buggy.shrunk->choices.empty());  // The plain "record was lost" crash is enough.
    CHECK(!ravel::replay(write_ahead_log_setup(false), buggy.shrunk->choices).ok);
  }

  options.shrink_first_failure = false;
  CHECK(ravel::run_seeds(write_ahead_log_setup(true), options).ok());
}

TEST(disk_injected_remove_errors_leave_the_file_in_place) {
  bool saw_error = false;
  for (std::uint64_t seed = 0; seed < 40; ++seed) {
    ravel::Simulation sim(seed);
    ravel::Disk& disk = sim.add_disk("d", {.write_error_probability = 0.5});
    ravel::DiskStatus removed = ravel::DiskStatus::Ok;
    run_with_disk(sim, disk, [&]() -> ravel::Task {
      // Writes fail too, so retry until the file exists.
      for (int attempt = 0; attempt < 64; ++attempt) {
        if (co_await disk.write("f", 0, "data") == ravel::DiskStatus::Ok) break;
      }
      removed = co_await disk.remove("f");
    });
    if (removed == ravel::DiskStatus::IoError) {
      saw_error = true;
      CHECK(disk.file_size("f") == 4);  // A failed remove changes nothing.
    } else {
      CHECK(removed == ravel::DiskStatus::Ok);
      CHECK(disk.file_size("f") == 0);
    }
  }
  CHECK(saw_error);
}

TEST(disk_failed_sync_keeps_writes_that_were_not_covered) {
  // Same timing as the test above: w2 lands while the sync is in flight, so it
  // is not covered by it and survives the failure.
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk(
      "d", {.latency_min = 10, .latency_max = 10, .sync_error_probability = 1.0});
  ravel::DiskStatus status = ravel::DiskStatus::Ok;
  std::string read_back;
  sim.scheduler().spawn("w1", [&]() -> ravel::Task { co_await disk.write("f", 0, "1"); });
  sim.scheduler().spawn("w2", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(8);
    co_await disk.write("f", 1, "2");
  });
  sim.scheduler().spawn("syncer", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(11);
    status = co_await disk.sync("f");
    read_back = (co_await disk.read("f", 0, 100)).data;
  });
  sim.run_until_quiescent();

  CHECK(status == ravel::DiskStatus::IoError);
  CHECK(disk.durable_contents("f").empty());
  CHECK(disk.file_size("f") == 2);  // w1 dropped, w2 kept: it starts at offset 1.
  CHECK((read_back == std::string("\0" "2", 2)));
}

TEST(disk_statuses_have_names) {
  CHECK(std::string(ravel::to_string(ravel::DiskStatus::Ok)) == "Ok");
  CHECK(std::string(ravel::to_string(ravel::DiskStatus::NoSpace)) == "NoSpace");
  CHECK(std::string(ravel::to_string(ravel::DiskStatus::IoError)) == "IoError");
  CHECK(std::string(ravel::to_string(ravel::DiskStatus::Crashed)) == "Crashed");
  CHECK(std::string(ravel::to_string(ravel::DiskStatus::NotFound)) == "NotFound");
}
