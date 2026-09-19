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
  NoSpace,   // The write would exceed the disk's capacity. Nothing was written.
  IoError,   // An injected fault. A failed write wrote nothing; a failed sync
             // dropped the writes it should have made durable.
  Crashed,   // The disk crashed while the operation was in flight.
};

const char* to_string(DiskStatus status) noexcept;

struct ReadResult {
  DiskStatus status = DiskStatus::Ok;
  std::string data;  // Shorter than asked if the read ran past the end of the file.
};

// Injectable disk behavior. Every decision is drawn from the Simulation's
// VirtualRng, so it replays identically for a given seed.
struct DiskFaultSpec {
  // Total bytes across all files. The default (1 GiB) only guards against a
  // stray offset asking for absurd amounts of memory.
  std::uint64_t capacity_bytes = 1ULL << 30;

  // Each operation takes a uniformly random time in [min, max].
  VirtualClock::Tick latency_min = 0;
  VirtualClock::Tick latency_max = 0;

  double write_error_probability = 0.0;  // In [0, 1].
  double sync_error_probability = 0.0;   // In [0, 1].
};

// A virtual disk holding named files, with the failure behavior that makes
// storage code hard to get right.
//
// Like a real disk with a page cache, a write is visible to reads at once but
// is only durable after sync(). crash() models power loss: every write that
// was not yet synced independently either vanishes, survives whole, or
// survives only in part (a torn write, cut at a 512-byte sector boundary).
// The outcome of each is a random choice, where the simplest (and what
// shrinking steers toward) is that the write is lost.
//
//   auto status = co_await disk.write("wal", 0, "record");
//   co_await disk.sync("wal");     // "record" now survives crash()
//
// All operations are awaited and take virtual time. Files are created by
// writing to them. There are no directories, and no real files are touched.
class Disk {
 public:
  static constexpr std::size_t kSectorSize = 512;

  Disk(DiskId id, std::string name, DiskFaultSpec fault, Scheduler& scheduler, VirtualRng& rng,
       Trace& trace);

  // Pending operations refer to this object, so it must never move.
  Disk(const Disk&) = delete;
  Disk& operator=(const Disk&) = delete;

  const std::string& name() const noexcept { return name_; }
  const DiskFaultSpec& fault() const noexcept { return fault_; }

  // An operation in flight; co_await it to get its result.
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

  // Writes `data` at `offset`, growing the file (with zero bytes) if needed.
  [[nodiscard]] Operation<DiskStatus> write(std::string path, std::uint64_t offset,
                                            std::string data);

  // Reads up to `length` bytes at `offset`, seeing every completed write.
  [[nodiscard]] Operation<ReadResult> read(std::string path, std::uint64_t offset,
                                           std::uint64_t length);

  // Makes every write that completed before this call was issued durable.
  [[nodiscard]] Operation<DiskStatus> sync(std::string path);

  // Power loss. Applies the crash rules above to unsynced writes and fails
  // every operation still in flight with DiskStatus::Crashed. Instant: call it
  // from a task, or from an invariant to inspect the aftermath.
  void crash();

  // The file's size as reads see it (0 if it does not exist).
  std::uint64_t file_size(const std::string& path) const;

  // The file as a crash right now would leave it at best: only synced data.
  std::string durable_contents(const std::string& path) const;

  std::uint64_t used_bytes() const;

 private:
  struct PendingWrite {
    std::uint64_t sequence;  // Completion order across the whole disk.
    std::uint64_t offset;
    std::string data;
  };

  struct File {
    std::string durable;
    std::vector<PendingWrite> pending;  // Completed but not yet synced.
    std::uint64_t logical_size = 0;     // What reads see: durable plus pending.
  };

  VirtualClock::Tick draw_latency() { return rng_.next_between(fault_.latency_min, fault_.latency_max); }
  void record(TraceEventKind kind) { trace_.record({scheduler_.now(), id_, kind}); }

  DiskStatus finish_write(const std::string& path, std::uint64_t offset, const std::string& data,
                          std::uint64_t epoch);
  ReadResult finish_read(const std::string& path, std::uint64_t offset, std::uint64_t length,
                         std::uint64_t epoch);
  DiskStatus finish_sync(const std::string& path, std::uint64_t horizon, std::uint64_t epoch);

  static void apply(std::string& bytes, std::uint64_t offset, const std::string& data);
  static void refresh_logical_size(File& file);

  DiskId id_;
  std::string name_;
  DiskFaultSpec fault_;
  Scheduler& scheduler_;
  VirtualRng& rng_;
  Trace& trace_;

  std::map<std::string, File> files_;      // Ordered, so crash() is deterministic.
  std::uint64_t completed_writes_ = 0;     // Sequence counter for PendingWrite.
  std::uint64_t epoch_ = 0;                // Bumped by crash(); tags operations.
};

}  // namespace ravel
