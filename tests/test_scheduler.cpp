#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ravel/clock.hpp"
#include "ravel/rng.hpp"
#include "ravel/scheduler.hpp"
#include "ravel/trace.hpp"
#include "testing.hpp"

namespace {

constexpr std::uint64_t kNoLimit = 1'000'000;

// Everything a Scheduler needs, so each test reads as "given a seed, do X".
struct Harness {
  explicit Harness(std::uint64_t seed) : rng(seed), scheduler(clock, rng, trace) {}

  ravel::VirtualClock clock;
  ravel::VirtualRng rng;
  ravel::Trace trace;
  ravel::Scheduler scheduler;
};

// Spawns three tasks that each log their letter twice, yielding in between,
// and returns the order in which the letters were logged.
std::string interleaving_for_seed(std::uint64_t seed) {
  Harness h(seed);
  std::string log;
  for (const char letter : {'a', 'b', 'c'}) {
    h.scheduler.spawn(std::string(1, letter), [&h, &log, letter]() -> ravel::Task {
      log += letter;
      co_await h.scheduler.yield();
      log += letter;
    });
  }
  h.scheduler.run_until_quiescent(kNoLimit);
  return log;
}

}  // namespace

TEST(scheduler_runs_every_task_to_completion) {
  Harness h(1);
  int finished = 0;
  for (int i = 0; i < 5; ++i) {
    h.scheduler.spawn("task", [&]() -> ravel::Task {
      ++finished;
      co_return;
    });
  }
  const ravel::RunReport report = h.scheduler.run_until_quiescent(kNoLimit);
  CHECK(report.status == ravel::RunStatus::Completed);
  CHECK(finished == 5);
}

TEST(scheduler_same_seed_gives_same_interleaving) {
  for (std::uint64_t seed = 0; seed < 20; ++seed) {
    CHECK(interleaving_for_seed(seed) == interleaving_for_seed(seed));
  }
}

TEST(scheduler_different_seeds_explore_different_interleavings) {
  std::set<std::string> distinct;
  for (std::uint64_t seed = 0; seed < 50; ++seed) distinct.insert(interleaving_for_seed(seed));
  CHECK(distinct.size() > 1);
}

TEST(scheduler_sleep_advances_virtual_time_only) {
  Harness h(1);
  ravel::VirtualClock::Tick woke_at = 0;
  h.scheduler.spawn("sleeper", [&]() -> ravel::Task {
    co_await h.scheduler.sleep(500);
    woke_at = h.clock.now();
  });
  h.scheduler.run_until_quiescent(kNoLimit);
  CHECK(woke_at == 500);
  CHECK(h.clock.now() == 500);
}

TEST(scheduler_wakes_sleepers_in_time_order) {
  Harness h(1);
  std::vector<int> wake_order;
  for (const int delay : {30, 10, 20}) {
    h.scheduler.spawn("sleeper", [&h, &wake_order, delay]() -> ravel::Task {
      co_await h.scheduler.sleep(delay);
      wake_order.push_back(delay);
    });
  }
  h.scheduler.run_until_quiescent(kNoLimit);
  CHECK((wake_order == std::vector<int>{10, 20, 30}));
}

TEST(scheduler_explores_order_of_simultaneous_wakeups) {
  std::set<std::string> distinct;
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    Harness h(seed);
    std::string log;
    for (const char letter : {'a', 'b', 'c'}) {
      h.scheduler.spawn("sleeper", [&h, &log, letter]() -> ravel::Task {
        co_await h.scheduler.sleep(10);
        log += letter;
      });
    }
    h.scheduler.run_until_quiescent(kNoLimit);
    distinct.insert(log);
  }
  CHECK(distinct.size() > 1);
}

TEST(scheduler_reports_a_task_that_throws) {
  Harness h(1);
  h.scheduler.spawn("bad_task", []() -> ravel::Task {
    throw std::runtime_error("boom");
    co_return;
  });
  const ravel::RunReport report = h.scheduler.run_until_quiescent(kNoLimit);
  CHECK(report.status == ravel::RunStatus::TaskThrew);
  CHECK(report.failure == "task 'bad_task' threw: boom");
}

TEST(scheduler_stops_a_livelock_at_the_step_limit) {
  Harness h(1);
  h.scheduler.spawn("spinner", [&]() -> ravel::Task {
    while (true) co_await h.scheduler.yield();
  });
  const ravel::RunReport report = h.scheduler.run_until_quiescent(100);
  CHECK(report.status == ravel::RunStatus::StepLimitReached);
  CHECK(report.steps == 100);
}

TEST(scheduler_lets_a_task_spawn_more_tasks) {
  Harness h(1);
  int finished = 0;
  h.scheduler.spawn("parent", [&]() -> ravel::Task {
    // Enough children to force the scheduler's task storage to grow while
    // this coroutine is suspended, which must not disturb any running task.
    for (int i = 0; i < 100; ++i) {
      h.scheduler.spawn("child", [&]() -> ravel::Task {
        co_await h.scheduler.yield();
        ++finished;
      });
    }
    ++finished;
    co_return;
  });
  const ravel::RunReport report = h.scheduler.run_until_quiescent(kNoLimit);
  CHECK(report.status == ravel::RunStatus::Completed);
  CHECK(finished == 101);
}

TEST(scheduler_records_the_task_lifecycle_in_the_trace) {
  Harness h(1);
  h.scheduler.spawn("only", []() -> ravel::Task { co_return; });
  h.scheduler.run_until_quiescent(kNoLimit);

  const auto& events = h.trace.events();
  CHECK(events.size() == 3);
  if (events.size() == 3) {
    CHECK(events[0].kind == ravel::TraceEventKind::TaskSpawned);
    CHECK(events[1].kind == ravel::TraceEventKind::TaskResumed);
    CHECK(events[2].kind == ravel::TraceEventKind::TaskFinished);
  }
  CHECK(h.scheduler.task_name(0) == "only");
}

TEST(scheduler_rejects_use_from_another_thread_while_running) {
  Harness h(1);
  bool rejected = false;
  h.scheduler.spawn("task_that_starts_a_thread", [&]() -> ravel::Task {
    std::thread intruder([&] {
      try {
        h.scheduler.spawn("intruder", []() -> ravel::Task { co_return; });
      } catch (const std::logic_error&) {
        rejected = true;
      }
    });
    intruder.join();
    co_return;
  });
  const ravel::RunReport report = h.scheduler.run_until_quiescent(kNoLimit);
  CHECK(report.status == ravel::RunStatus::Completed);
  CHECK(rejected);
}

TEST(scheduler_can_be_set_up_on_one_thread_and_run_on_another) {
  Harness h(1);
  int ran = 0;
  h.scheduler.spawn("task", [&]() -> ravel::Task {
    ++ran;
    co_return;
  });
  std::thread runner([&] { h.scheduler.run_until_quiescent(kNoLimit); });
  runner.join();
  CHECK(ran == 1);
}
