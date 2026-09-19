#pragma once

#include <coroutine>
#include <exception>
#include <utility>

namespace ravel {

/// The coroutine type for simulated tasks. A task starts suspended and runs
/// only when the Scheduler resumes it; every `co_await` inside it is a point
/// where the Scheduler may switch to another task.
///
///   ravel::Task worker(ravel::Scheduler& scheduler) {
///     co_await scheduler.sleep(10);
///     ...
///   }
///
/// Task is move-only and owns its coroutine frame.
class Task {
 public:
  struct promise_type {
    Task get_return_object() noexcept {
      return Task(std::coroutine_handle<promise_type>::from_promise(*this));
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    std::suspend_always final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() noexcept { exception = std::current_exception(); }

    std::exception_ptr exception;
  };

  Task() noexcept = default;
  /// Tasks are move-only: a Task owns its coroutine.
  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  /// Tasks are move-only: a Task owns its coroutine.
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() { destroy(); }

  /// True once the coroutine ran to completion or threw. An empty Task counts
  /// as done.
  bool done() const noexcept { return !handle_ || handle_.done(); }

  /// Runs the coroutine to its next suspension point.
  void resume() { handle_.resume(); }

  /// The exception that escaped the coroutine body, if any.
  std::exception_ptr exception() const noexcept {
    return handle_ ? handle_.promise().exception : nullptr;
  }

 private:
  explicit Task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

  void destroy() noexcept {
    if (handle_) handle_.destroy();
    handle_ = {};
  }

  std::coroutine_handle<promise_type> handle_;
};

}  // namespace ravel
