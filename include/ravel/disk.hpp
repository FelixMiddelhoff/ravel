#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ravel/clock.hpp"
#include "ravel/rng.hpp"
#include "ravel/scheduler.hpp"
#include "ravel/trace.hpp"

namespace ravel {

using DiskId = std::size_t;

enum class DiskStatus {
  Ok,
  NoSpace,   ///< The write would exceed the disk's capacity. Nothing was written.
  IoError,   ///< An injected fault. A failed write, rename or remove changed
             ///< nothing; a failed sync of a file dropped the writes it should
             ///< have made durable.
  Crashed,   ///< The disk crashed while the operation was in flight.
  NotFound,  ///< The file or directory does not exist.
};

const char* to_string(DiskStatus status) noexcept;

/// The outcome of Disk::read.
struct ReadResult {
  /// Ok, or why the read failed (for example NotFound).
  DiskStatus status = DiskStatus::Ok;
  std::string data;  ///< Shorter than asked if the read ran past the end of the file.
};

/// The outcome of Disk::list.
struct ListResult {
  /// Ok, or why the listing failed.
  DiskStatus status = DiskStatus::Ok;
  std::vector<std::string> names;  ///< Sorted; a subdirectory is listed as "name/".
};

/// Injectable disk behavior. Every decision is drawn from the Simulation's
/// VirtualRng, so it replays identically for a given seed.
struct DiskFaultSpec {
  /// Total bytes across all files. The default (1 GiB) only guards against a
  /// stray offset asking for absurd amounts of memory.
  std::uint64_t capacity_bytes = 1ULL << 30;

  /// Each operation takes a uniformly random time in [min, max].
  VirtualClock::Tick latency_min = 0;
  /// The upper end of the range above.
  VirtualClock::Tick latency_max = 0;

  /// In [0, 1]. Writes, renames and removes fail with IoError, changing nothing.
  double write_error_probability = 0.0;

  /// In [0, 1]. Syncing a file fails with IoError and drops the data it should
  /// have flushed; syncing a directory fails with IoError and changes nothing.
  double sync_error_probability = 0.0;
};

/// A virtual disk holding named files, with the failure behavior that makes
/// storage code hard to get right.
///
/// Data. Like a real disk with a page cache, a write is visible to reads at
/// once but is only durable after sync(path). crash() models power loss: every
/// write that was not yet synced independently either vanishes, survives whole,
/// or survives only in part (a torn write, cut at a 512-byte sector boundary).
///
/// Names. Creating a file, renaming it and removing it are visible at once too,
/// but each changes the directory only in memory until it is made durable.
/// Directory changes reach the disk in the order they were made, so a crash
/// keeps some prefix of the pending ones (possibly none). Two calls make them
/// durable:
///   * sync(path) also makes the file's own creation durable, as fsync does on
///     ext4 and xfs, but not renames or removals;
///   * sync_dir(dir) makes durable every change up to the last one that touched
///     that directory.
/// So a file that was written and renamed without syncing can come back after
/// a crash under its old name, or under the new one with none of its data.
/// The safe way to replace a file is: write a temporary file, sync it, rename
/// it over the target, sync the directory.
///
/// The outcome of each crash decision is a random choice, and the simplest
/// outcome (what shrinking steers toward) is always that the change is lost.
///
///   co_await disk.write("wal/log", 0, "record");
///   co_await disk.sync("wal/log");    // "record" now survives crash()
///
/// Paths use '/' as the separator and have no leading slash. Directories exist
/// implicitly: a directory is there as long as a file is under it, and "" is
/// the root. All operations are awaited and take virtual time. No real files
/// are touched.
class Disk {
 public:
  /// A write torn by a crash is cut at a multiple of this many bytes.
  static constexpr std::size_t kSectorSize = 512;

  /// Created by Simulation::add_disk; you do not construct one yourself.
  Disk(DiskId id, std::string name, DiskFaultSpec fault, Scheduler& scheduler, VirtualRng& rng,
       Trace& trace);

  /// Pending operations refer to this object, so it must never move.
  Disk(const Disk&) = delete;
  Disk& operator=(const Disk&) = delete;

  /// The name given to add_disk.
  const std::string& name() const noexcept { return name_; }
  /// The fault settings this disk was created with.
  const DiskFaultSpec& fault() const noexcept { return fault_; }

  /// An operation in flight; co_await it to get its result.
  template <typename Result>
  class Operation {
   public:
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) {
      Scheduler& scheduler = disk_.scheduler_;
      scheduler.call_after(disk_.draw_latency(), [this, task = scheduler.current_task()] {
        result_ = finish_();
        disk_.scheduler_.make_runnable(task);
      });
    }
    Result await_resume() { return std::move(result_); }

   private:
    friend class Disk;
    Operation(Disk& disk, std::function<Result()> finish)
        : disk_(disk), finish_(std::move(finish)) {}

    Disk& disk_;
    std::function<Result()> finish_;
    Result result_{};
  };

  /// Writes `data` at `offset`, creating the file if it does not exist and
  /// growing it (with zero bytes) if needed.
  [[nodiscard]] Operation<DiskStatus> write(std::string path, std::uint64_t offset,
                                            std::string data);

  /// Reads up to `length` bytes at `offset`, seeing every completed write.
  /// NotFound if the file does not exist.
  [[nodiscard]] Operation<ReadResult> read(std::string path, std::uint64_t offset,
                                           std::uint64_t length);

  /// Makes every write to the file that completed before this call was issued
  /// durable, and the file's creation (see above).
  [[nodiscard]] Operation<DiskStatus> sync(std::string path);

  /// Renames a file, replacing `to` if it exists. NotFound if `from` does not.
  [[nodiscard]] Operation<DiskStatus> rename(std::string from, std::string to);

  /// Removes a file. NotFound if it does not exist.
  [[nodiscard]] Operation<DiskStatus> remove(std::string path);

  /// Makes durable every directory change, made before this call was issued,
  /// up to the last one that touched `dir` (a file created, renamed into or out
  /// of it, or removed from it).
  [[nodiscard]] Operation<DiskStatus> sync_dir(std::string dir);

  /// The entries directly inside `dir`. NotFound if `dir` has none (except the
  /// root, which may be empty).
  [[nodiscard]] Operation<ListResult> list(std::string dir);

  /// Power loss. Applies the crash rules above to unsynced writes and
  /// directory changes, and fails every operation still in flight with
  /// DiskStatus::Crashed. Instant: call it from a task, or from an invariant
  /// to inspect the aftermath.
  void crash();

  /// How many times crash() has been called. A process that a crash kills can
  /// read this before starting and stop once it changes: an operation that
  /// completed just before the crash is not enough to keep going.
  std::uint64_t crash_count() const noexcept { return epoch_; }

  /// The file's size as reads see it (0 if it does not exist).
  std::uint64_t file_size(const std::string& path) const;

  /// The file as a crash right now would leave it at best: only durable data,
  /// and empty unless the file's name is durable too.
  std::string durable_contents(const std::string& path) const;

  /// Whether the file's name would survive a crash right now.
  bool durable_exists(const std::string& path) const;

  /// Total bytes of all files, as reads see them.
  std::uint64_t used_bytes() const;

 private:
  using InodeId = std::uint64_t;

  struct PendingWrite {
    std::uint64_t sequence;  ///< Completion order across the whole disk.
    std::uint64_t offset;
    std::string data;
  };

  struct Inode {
    std::string durable;
    std::vector<PendingWrite> pending;  ///< Completed but not yet synced.
    std::uint64_t logical_size = 0;     ///< What reads see: durable plus pending.
  };

  enum class NameChange { Create, Rename, Remove };

  /// A directory change that has completed but is not yet durable.
  struct PendingNameChange {
    std::uint64_t sequence;
    NameChange kind;
    std::string path;    ///< The file created, the old name, or the file removed.
    std::string target;  ///< The new name, for a rename.
    InodeId inode;
  };

  using Names = std::map<std::string, InodeId>;

  VirtualClock::Tick draw_latency() {
    return rng_.next_between(fault_.latency_min, fault_.latency_max);
  }
  void record(TraceEventKind kind) { trace_.record({scheduler_.now(), id_, kind}); }

  DiskStatus finish_write(const std::string& path, std::uint64_t offset, const std::string& data,
                          std::uint64_t epoch);
  ReadResult finish_read(const std::string& path, std::uint64_t offset, std::uint64_t length,
                         std::uint64_t epoch);
  DiskStatus finish_sync(const std::string& path, std::uint64_t horizon, std::uint64_t epoch);
  DiskStatus finish_rename(const std::string& from, const std::string& to, std::uint64_t epoch);
  DiskStatus finish_remove(const std::string& path, std::uint64_t epoch);
  DiskStatus finish_sync_dir(const std::string& dir, std::uint64_t horizon, std::uint64_t epoch);
  ListResult finish_list(const std::string& dir, std::uint64_t epoch);

  /// Moves the first `count` pending name changes into the durable names.
  void make_durable(std::size_t count);

  static void apply(std::string& bytes, std::uint64_t offset, const std::string& data);
  static void apply(Names& names, const PendingNameChange& change);
  static void refresh_logical_size(Inode& inode);
  static std::string parent_of(const std::string& path);

  DiskId id_;
  std::string name_;
  DiskFaultSpec fault_;
  Scheduler& scheduler_;
  VirtualRng& rng_;
  Trace& trace_;

  std::map<InodeId, Inode> inodes_;        ///< Ordered, so crash() is deterministic.
  Names visible_names_;                    ///< What reads and listings see.
  Names durable_names_;                    ///< What a crash keeps of the directory.
  std::vector<PendingNameChange> pending_names_;
  InodeId next_inode_ = 1;
  std::uint64_t next_sequence_ = 0;        ///< Orders writes and name changes.
  std::uint64_t epoch_ = 0;                ///< Bumped by crash(); tags operations.
};

}  // namespace ravel
