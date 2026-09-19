// Directory behavior of the virtual disk: rename, remove, list, sync_dir, and
// what a crash does to names that were never made durable.
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "ravel/runner.hpp"
#include "ravel/shrink.hpp"
#include "testing.hpp"

namespace {

// Runs `body` as the only task of `sim`.
template <typename Body>
void run_task(ravel::Simulation& sim, Body body) {
  sim.scheduler().spawn("test", std::move(body));
  sim.run_until_quiescent();
}

}  // namespace

TEST(disk_rename_is_visible_at_once_and_replaces_the_target) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  ravel::DiskStatus renamed = ravel::DiskStatus::IoError;
  ravel::DiskStatus old_name = ravel::DiskStatus::Ok;
  std::string new_contents;
  run_task(sim, [&]() -> ravel::Task {
    co_await disk.write("a", 0, "from a");
    co_await disk.write("b", 0, "old b");
    renamed = co_await disk.rename("a", "b");
    old_name = (co_await disk.read("a", 0, 100)).status;
    new_contents = (co_await disk.read("b", 0, 100)).data;
  });
  CHECK(renamed == ravel::DiskStatus::Ok);
  CHECK(old_name == ravel::DiskStatus::NotFound);
  CHECK(new_contents == "from a");
  CHECK(disk.used_bytes() == 6);  // "old b" was unlinked and no longer counts.
}

TEST(disk_reports_missing_files_as_not_found) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  ravel::DiskStatus rename_status = ravel::DiskStatus::Ok;
  ravel::DiskStatus remove_status = ravel::DiskStatus::Ok;
  ravel::DiskStatus sync_status = ravel::DiskStatus::Ok;
  ravel::DiskStatus list_status = ravel::DiskStatus::Ok;
  run_task(sim, [&]() -> ravel::Task {
    rename_status = co_await disk.rename("nope", "other");
    remove_status = co_await disk.remove("nope");
    sync_status = co_await disk.sync("nope");
    list_status = (co_await disk.list("nodir")).status;
  });
  CHECK(rename_status == ravel::DiskStatus::NotFound);
  CHECK(remove_status == ravel::DiskStatus::NotFound);
  CHECK(sync_status == ravel::DiskStatus::NotFound);
  CHECK(list_status == ravel::DiskStatus::NotFound);
}

TEST(disk_remove_deletes_a_file) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  ravel::DiskStatus after = ravel::DiskStatus::Ok;
  run_task(sim, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "data");
    co_await disk.remove("f");
    after = (co_await disk.read("f", 0, 10)).status;
  });
  CHECK(after == ravel::DiskStatus::NotFound);
  CHECK(disk.file_size("f") == 0);
  CHECK(disk.used_bytes() == 0);
}

TEST(disk_lists_entries_directly_inside_a_directory) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  std::vector<std::string> root;
  std::vector<std::string> wal;
  run_task(sim, [&]() -> ravel::Task {
    co_await disk.write("top", 0, "1");
    co_await disk.write("wal/000", 0, "1");
    co_await disk.write("wal/001", 0, "1");
    co_await disk.write("wal/old/x", 0, "1");
    root = (co_await disk.list("")).names;
    wal = (co_await disk.list("wal")).names;
  });
  CHECK((root == std::vector<std::string>{"top", "wal/"}));
  CHECK((wal == std::vector<std::string>{"000", "001", "old/"}));
}

TEST(disk_listing_the_empty_root_is_fine) {
  ravel::Simulation sim(1);
  ravel::Disk& disk = sim.add_disk("d");
  ravel::ListResult result{ravel::DiskStatus::IoError, {}};
  run_task(sim, [&]() -> ravel::Task { result = co_await disk.list(""); });
  CHECK(result.status == ravel::DiskStatus::Ok);
  CHECK(result.names.empty());
}

TEST(disk_crash_can_forget_an_unsynced_rename) {
  const auto durable_names_after_crash = [](ravel::Choices choices) {
    ravel::Simulation sim(0, ravel::SimulationOptions{.replay_choices = std::move(choices)});
    ravel::Disk& disk = sim.add_disk("d");
    run_task(sim, [&]() -> ravel::Task {
      co_await disk.write("a", 0, "x");
      co_await disk.sync("a");         // "a" is durable.
      co_await disk.rename("a", "b");  // The rename is not.
      disk.crash();
    });
    return std::string(disk.durable_exists("a") ? "a" : "") +
           (disk.durable_exists("b") ? "b" : "");
  };
  CHECK(durable_names_after_crash({0}) == "a");  // The rename was lost.
  CHECK(durable_names_after_crash({1}) == "b");  // It made it to the disk.
}

TEST(disk_sync_dir_makes_a_rename_durable) {
  for (std::uint64_t seed = 0; seed < 30; ++seed) {
    ravel::Simulation sim(seed);
    ravel::Disk& disk = sim.add_disk("d");
    run_task(sim, [&]() -> ravel::Task {
      co_await disk.write("a", 0, "x");
      co_await disk.sync("a");
      co_await disk.rename("a", "b");
      co_await disk.sync_dir("");
      disk.crash();
    });
    CHECK(disk.durable_exists("b"));
    CHECK(!disk.durable_exists("a"));
    CHECK(disk.durable_contents("b") == "x");
  }
}

TEST(disk_sync_of_a_file_makes_its_creation_durable_but_not_its_rename) {
  std::set<std::string> outcomes;
  for (std::uint64_t seed = 0; seed < 40; ++seed) {
    ravel::Simulation sim(seed);
    ravel::Disk& disk = sim.add_disk("d");
    run_task(sim, [&]() -> ravel::Task {
      co_await disk.write("x", 0, "data");
      co_await disk.rename("x", "y");
      co_await disk.sync("y");  // The file's creation is durable; the rename is not.
      disk.crash();
    });
    // The file always exists, under exactly one of its two names.
    const bool old_name = disk.durable_exists("x");
    CHECK(old_name != disk.durable_exists("y"));
    CHECK(disk.durable_contents(old_name ? "x" : "y") == "data");
    outcomes.insert(old_name ? "x" : "y");
  }
  CHECK(outcomes.size() == 2);  // Both fates are explored.
}

TEST(disk_sync_dir_makes_changes_durable_in_order_up_to_the_directory) {
  const auto run = [](const char* synced_dir) {
    ravel::Simulation sim(0);
    ravel::Disk& disk = sim.add_disk("d");
    run_task(sim, [&disk, synced_dir]() -> ravel::Task {
      co_await disk.write("a/f", 0, "1");
      co_await disk.write("b/g", 0, "2");
      co_await disk.sync("a/f");
      co_await disk.sync("b/g");
      co_await disk.rename("a/f", "a/f2");  // Change 1, in directory a.
      co_await disk.rename("b/g", "b/g2");  // Change 2, in directory b.
      co_await disk.sync_dir(synced_dir);
    });
    return std::string(disk.durable_exists("a/f2") ? "A" : "-") +
           (disk.durable_exists("b/g2") ? "B" : "-");
  };
  CHECK(run("a") == "A-");  // Syncing a covers change 1 only.
  CHECK(run("b") == "AB");  // Syncing b must carry change 1 along, to keep order.
}

TEST(disk_crash_can_forget_an_unsynced_remove) {
  const auto survives = [](ravel::Choices choices) {
    ravel::Simulation sim(0, ravel::SimulationOptions{.replay_choices = std::move(choices)});
    ravel::Disk& disk = sim.add_disk("d");
    run_task(sim, [&]() -> ravel::Task {
      co_await disk.write("f", 0, "x");
      co_await disk.sync("f");
      co_await disk.remove("f");
      disk.crash();
    });
    return disk.durable_exists("f");
  };
  CHECK(survives({0}));  // The removal was lost: the file is back.
  CHECK(!survives({1}));
}

TEST(disk_crash_forgets_files_whose_creation_never_became_durable) {
  ravel::Simulation sim(0, ravel::SimulationOptions{.replay_choices = ravel::Choices{}});
  ravel::Disk& disk = sim.add_disk("d");
  run_task(sim, [&]() -> ravel::Task {
    co_await disk.write("f", 0, "data");
    disk.crash();  // Every choice defaults to 0: nothing unsynced survives.
  });
  CHECK(disk.file_size("f") == 0);
  CHECK(!disk.durable_exists("f"));
}

// The textbook way to replace a file atomically, and the two ways to get it
// wrong. A power cut can come at any time; afterwards "conf" must hold the old
// version or the new one, never nothing and never a mix, and once the update
// was acknowledged it must be the new one.
namespace {

enum class ReplaceBug { None, SkipFileSync, SkipDirSync };

ravel::SimulationSetup atomic_replace_setup(ReplaceBug bug) {
  return [bug](ravel::Simulation& sim) {
    ravel::Disk& disk = sim.add_disk("d", {.latency_min = 1, .latency_max = 5});
    bool& acknowledged = sim.make_state<bool>(false);
    bool& ready = sim.make_state<bool>(false);

    sim.scheduler().spawn("updater", [&disk, &acknowledged, &ready, bug]() -> ravel::Task {
      // A process that loses power is gone: it must not carry on, however its
      // last operation ended. That includes one that completed a moment
      // before the crash.
      const std::uint64_t boot = disk.crash_count();
      const auto ok = [&disk, boot](ravel::DiskStatus status) {
        return status == ravel::DiskStatus::Ok && disk.crash_count() == boot;
      };

      if (!ok(co_await disk.write("conf", 0, "version 1"))) co_return;
      if (!ok(co_await disk.sync("conf"))) co_return;
      if (!ok(co_await disk.sync_dir(""))) co_return;
      ready = true;

      if (!ok(co_await disk.write("conf.tmp", 0, "version 2"))) co_return;
      if (bug != ReplaceBug::SkipFileSync && !ok(co_await disk.sync("conf.tmp"))) co_return;
      if (!ok(co_await disk.rename("conf.tmp", "conf"))) co_return;
      if (bug != ReplaceBug::SkipDirSync && !ok(co_await disk.sync_dir(""))) co_return;
      acknowledged = true;
    });
    sim.scheduler().spawn("power_cut", [&sim, &disk]() -> ravel::Task {
      co_await sim.scheduler().sleep(15 + sim.rng().next_below(30));
      disk.crash();
    });
    sim.add_invariant("conf_is_old_or_new_never_broken", [&disk, &ready] {
      if (!ready) return true;  // The crash came before the first version existed.
      const std::string conf = disk.durable_contents("conf");
      return conf == "version 1" || conf == "version 2";
    });
    sim.add_invariant("acknowledged_update_is_durable", [&disk, &acknowledged] {
      return !acknowledged || disk.durable_contents("conf") == "version 2";
    });
  };
}

}  // namespace

TEST(disk_atomic_replace_survives_a_power_cut_at_any_time) {
  ravel::RunnerOptions options;
  options.seed_count = 400;
  CHECK(ravel::run_seeds(atomic_replace_setup(ReplaceBug::None), options).ok());
}

TEST(disk_finds_the_missing_file_sync_before_rename) {
  ravel::RunnerOptions options;
  options.seed_count = 400;
  const ravel::RunnerReport report =
      ravel::run_seeds(atomic_replace_setup(ReplaceBug::SkipFileSync), options);
  CHECK(!report.ok());  // "conf" can end up empty or torn.
}

TEST(disk_finds_the_missing_directory_sync_after_rename) {
  ravel::RunnerOptions options;
  options.seed_count = 400;
  options.shrink_first_failure = true;
  const ravel::RunnerReport report =
      ravel::run_seeds(atomic_replace_setup(ReplaceBug::SkipDirSync), options);
  CHECK(!report.ok());
  CHECK(report.shrunk.has_value());
  if (report.shrunk) {
    CHECK(report.shrunk->choices.size() <= report.shrunk->original_choices.size());
    const ravel::Result replayed =
        ravel::replay(atomic_replace_setup(ReplaceBug::SkipDirSync), report.shrunk->choices);
    CHECK(!replayed.ok);
  }
}
