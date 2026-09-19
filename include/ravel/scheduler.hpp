#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "ravel/clock.hpp"
#include "ravel/rng.hpp"
#include "ravel/task.hpp"
#include "ravel/trace.hpp"

namespace ravel {

using TaskId = std::size_t;

// Creates a task's coroutine. The Scheduler keeps the factory alive for as
// long as the task lives, so a capturing lambda is safe:
//
//   scheduler.spawn("worker", [&]() -> ravel::Task { co_await ...; });
//
// (A coroutine lambda normally must outlive its coroutine; storing the
// factory here is what guarantees that.)
using TaskFactory = std::function<Task()>;

enum class RunStatus {
  Completed,         // Every task finished.
  TaskThrew,         // A task let an exception escape; the run stopped there.
  StepLimitReached,  // Still runnable work after `max_steps` (likely a livelock).
};

struct RunReport {
  RunStatus status = RunStatus::Completed;
  std::uint64_t steps = 0;  // How many times a task was resumed.
  std::string failure;      // Human-readable cause; empty when Completed.
};

// Cooperative, single-threaded task scheduler. At every step it resumes one
// runnable task chosen by the seeded VirtualRng, so which task runs when
// depends only on the seed: never on wall-clock timing, thread scheduling, or
// hash-table iteration order. That is what makes a failing seed replayable,
// and what explores different interleavings across different seeds.
//
// Time is virtual. When every task is asleep the clock jumps straight to the
// earliest wake-up, so simulated waiting costs no real time.
class Scheduler {
 public:
  Scheduler(VirtualClock& clock, VirtualRng& rng, Trace& trace) noexcept
      : clock_(clock), rng_(rng), trace_(trace) {}

  // Tasks hold references into the scheduler, so it must never move.
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // Registers a task. It first runs on a later step of run_until_quiescent()
  // (or on the current run, if called from inside a task).
  void spawn(std::string name, TaskFactory factory);

  // Awaitables for use inside a task:
  //   co_await scheduler.yield();     // let the scheduler pick who runs next
  //   co_await scheduler.sleep(50);   // suspend for 50 virtual ticks

  class YieldAwaiter {
   public:
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const { scheduler_.requeue_current_task(); }
    void await_resume() const noexcept {}

   private:
    friend class Scheduler;
    explicit YieldAwaiter(Scheduler& scheduler) noexcept : scheduler_(scheduler) {}
    Scheduler& scheduler_;
  };

  class SleepAwaiter {
   public:
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const {
      scheduler_.sleep_current_task(duration_);
    }
    void await_resume() const noexcept {}

   private:
    friend class Scheduler;
    SleepAwaiter(Scheduler& scheduler, VirtualClock::Tick duration) noexcept
        : scheduler_(scheduler), duration_(duration) {}
    Scheduler& scheduler_;
    VirtualClock::Tick duration_;
  };

  [[nodiscard]] YieldAwaiter yield() noexcept { return YieldAwaiter(*this); }
  [[nodiscard]] SleepAwaiter sleep(VirtualClock::Tick duration) noexcept {
    return SleepAwaiter(*this, duration);
  }

  // Runs until every task has finished, a task throws, or `max_steps` steps
  // have been taken.
  RunReport run_until_quiescent(std::uint64_t max_steps);

  const std::string& task_name(TaskId id) const { return slots_.at(id).name; }

 private:
  struct TaskSlot {
    TaskSlot(std::string task_name, TaskFactory task_factory)
        : name(std::move(task_name)), factory(std::move(task_factory)) {}

    std::string name;
    TaskFactory factory;  // Declared before `task`: the coroutine may use it,
    Task task;            // so it must be destroyed after the coroutine.
  };

  struct Timer {
    VirtualClock::Tick wake_at;
    std::uint64_t sequence;  // Creation order; makes equal wake times ordered.
    TaskId task;
  };

  struct WakesLater {
    bool operator()(const Timer& a, const Timer& b) const noexcept {
      return a.wake_at != b.wake_at ? a.wake_at > b.wake_at : a.sequence > b.sequence;
    }
  };

  void requeue_current_task() { runnable_.push_back(current_task_); }
  void sleep_current_task(VirtualClock::Tick duration);

  bool has_pending_work() const noexcept { return !runnable_.empty() || !timers_.empty(); }
  void wake_earliest_timers();
  TaskId take_random_runnable_task();

  // Runs one task until its next suspension point. Returns the exception the
  // task let escape, or null.
  std::exception_ptr resume_task(TaskId id);

  void record(TaskId task, TraceEventKind kind) {
    trace_.record({clock_.now(), task, kind});
  }

  VirtualClock& clock_;
  VirtualRng& rng_;
  Trace& trace_;

  // A deque, not a vector: growing it must not move existing slots, because a
  // running coroutine refers to the factory stored in its slot.
  std::deque<TaskSlot> slots_;
  std::vector<TaskId> runnable_;
  std::priority_queue<Timer, std::vector<Timer>, WakesLater> timers_;
  std::uint64_t next_timer_sequence_ = 0;
  TaskId current_task_ = 0;  // Valid only while a task is being resumed.
};

}  // namespace ravel
