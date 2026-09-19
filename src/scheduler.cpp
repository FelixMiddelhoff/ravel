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

void Scheduler::call_after(VirtualClock::Tick delay, std::function<void()> action) {
  constexpr auto kMaxTick = std::numeric_limits<VirtualClock::Tick>::max();
  const VirtualClock::Tick now = clock_.now();
  const VirtualClock::Tick due = delay > kMaxTick - now ? kMaxTick : now + delay;
  timers_.push({due, next_timer_sequence_++, std::move(action)});
}

void Scheduler::sleep_current_task(VirtualClock::Tick duration) {
  call_after(duration, [this, task = current_task_] { make_runnable(task); });
}

// Advances the clock to the earliest timer and runs every timer due then. A
// sleeper's timer just makes it runnable, so all simultaneous wake-ups become
// runnable together and the random pick that follows explores their order.
void Scheduler::fire_earliest_timers() {
  const VirtualClock::Tick due = timers_.top().due;
  clock_.advance_to(due);
  while (!timers_.empty() && timers_.top().due == due) {
    const Timer timer = timers_.top();  // priority_queue only allows copying out.
    timers_.pop();
    timer.action();
  }
}

// Choice 0 is the task that has been runnable longest, so a run whose choices
// are all 0 is plain round-robin: the simplest schedule, which shrinking
// steers toward. That is why the queue keeps its order on removal.
TaskId Scheduler::take_random_runnable_task() {
  const std::size_t index = rng_.next_below(runnable_.size());
  const TaskId id = runnable_[index];
  runnable_.erase(runnable_.begin() + static_cast<std::ptrdiff_t>(index));
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
    if (runnable_.empty()) {
      fire_earliest_timers();  // May run actions that wake nobody; look again.
      continue;
    }

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
