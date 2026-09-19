#include "ravel/scheduler.hpp"

#include <limits>
#include <utility>

namespace ravel {

namespace {

std::string describe(const std::exception_ptr& error) {
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& e) {
    return e.what();
  } catch (...) {
    return "unknown exception";
  }
}

}  // namespace

void Scheduler::spawn(std::string name, TaskFactory factory) {
  const TaskId id = slots_.size();
  TaskSlot& slot = slots_.emplace_back(std::move(name), std::move(factory));
  slot.task = slot.factory();  // Called on the stored factory, see TaskFactory.
  runnable_.push_back(id);
  record(id, TraceEventKind::TaskSpawned);
}

void Scheduler::sleep_current_task(VirtualClock::Tick duration) {
  constexpr auto kMaxTick = std::numeric_limits<VirtualClock::Tick>::max();
  const VirtualClock::Tick now = clock_.now();
  const VirtualClock::Tick wake_at = duration > kMaxTick - now ? kMaxTick : now + duration;
  timers_.push({wake_at, next_timer_sequence_++, current_task_});
}

// Makes every timer due at the earliest wake time runnable at once, so the
// random pick that follows also explores the order of simultaneous wake-ups.
void Scheduler::wake_earliest_timers() {
  const VirtualClock::Tick wake_at = timers_.top().wake_at;
  clock_.advance_to(wake_at);
  while (!timers_.empty() && timers_.top().wake_at == wake_at) {
    runnable_.push_back(timers_.top().task);
    timers_.pop();
  }
}

TaskId Scheduler::take_random_runnable_task() {
  const std::size_t index = rng_.next_below(runnable_.size());
  const TaskId id = runnable_[index];
  runnable_[index] = runnable_.back();
  runnable_.pop_back();
  return id;
}

std::exception_ptr Scheduler::resume_task(TaskId id) {
  Task& task = slots_[id].task;

  record(id, TraceEventKind::TaskResumed);
  current_task_ = id;
  task.resume();

  if (!task.done()) return nullptr;  // It re-queued or put itself to sleep.

  std::exception_ptr error = task.exception();
  record(id, error ? TraceEventKind::TaskThrew : TraceEventKind::TaskFinished);
  task = Task();  // Free the coroutine frame now that it is finished.
  return error;
}

RunReport Scheduler::run_until_quiescent(std::uint64_t max_steps) {
  RunReport report;

  while (has_pending_work()) {
    if (runnable_.empty()) wake_earliest_timers();

    if (report.steps == max_steps) {
      report.status = RunStatus::StepLimitReached;
      report.failure = "step limit of " + std::to_string(max_steps) +
                       " reached (tasks are still runnable; possible livelock)";
      return report;
    }

    const TaskId id = take_random_runnable_task();
    ++report.steps;

    if (const std::exception_ptr error = resume_task(id)) {
      report.status = RunStatus::TaskThrew;
      report.failure = "task '" + slots_[id].name + "' threw: " + describe(error);
      return report;
    }
  }

  return report;
}

}  // namespace ravel
