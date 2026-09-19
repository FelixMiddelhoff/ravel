// Receiving with a timeout, clearing an inbox, and stopping a run at a time
// limit: what a system that never goes quiet (heartbeats, elections) needs.
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ravel/simulation.hpp"
#include "testing.hpp"

TEST(receive_within_returns_a_message_that_arrives_in_time) {
  ravel::Simulation sim(1);
  auto& link = sim.add_channel("a", "b", {.latency_min = 10, .latency_max = 10});
  std::optional<ravel::Message> received;
  ravel::VirtualClock::Tick at = 0;
  sim.scheduler().spawn("sender", [&]() -> ravel::Task {
    link.send("hello");
    co_return;
  });
  sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
    received = co_await link.receive_within(50);
    at = sim.clock().now();
  });
  CHECK(sim.run_until_quiescent().ok);
  CHECK(received == std::optional<ravel::Message>("hello"));
  CHECK(at == 10);
}

TEST(receive_within_gives_up_after_the_timeout) {
  ravel::Simulation sim(1);
  auto& link = sim.add_channel("a", "b", {});
  std::optional<ravel::Message> received = "unset";
  ravel::VirtualClock::Tick at = 0;
  sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
    received = co_await link.receive_within(30);
    at = sim.clock().now();
  });
  CHECK(sim.run_until_quiescent().ok);
  CHECK(!received.has_value());
  CHECK(at == 30);
}

TEST(a_message_that_arrives_after_the_timeout_waits_for_the_next_receive) {
  ravel::Simulation sim(1);
  auto& link = sim.add_channel("a", "b", {.latency_min = 40, .latency_max = 40});
  std::optional<ravel::Message> first = "unset";
  std::optional<ravel::Message> second;
  sim.scheduler().spawn("sender", [&]() -> ravel::Task {
    link.send("late");
    co_return;
  });
  sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
    first = co_await link.receive_within(10);   // Gives up at t=10.
    second = co_await link.receive_within(100); // "late" arrives at t=40.
  });
  CHECK(sim.run_until_quiescent().ok);
  CHECK(!first.has_value());
  CHECK(second == std::optional<ravel::Message>("late"));
}

TEST(a_timeout_left_over_from_an_earlier_wait_does_not_wake_a_later_one) {
  // The first wait ends at t=5 with a message, leaving a timer set for t=100.
  // The second wait starts at t=5 and must not be cut short by that stale
  // timer: it should last until its own timeout at t=205.
  ravel::Simulation sim(1);
  auto& link = sim.add_channel("a", "b", {.latency_min = 5, .latency_max = 5});
  ravel::VirtualClock::Tick second_ended_at = 0;
  std::optional<ravel::Message> second = "unset";
  sim.scheduler().spawn("sender", [&]() -> ravel::Task {
    link.send("one");
    co_return;
  });
  sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
    co_await link.receive_within(100);
    second = co_await link.receive_within(200);
    second_ended_at = sim.clock().now();
  });
  CHECK(sim.run_until_quiescent().ok);
  CHECK(!second.has_value());
  CHECK(second_ended_at == 205);
}

TEST(clear_inbox_drops_delivered_messages) {
  ravel::Simulation sim(1);
  auto& link = sim.add_channel("a", "b", {});
  std::optional<ravel::Message> after_clear = "unset";
  sim.scheduler().spawn("sender", [&]() -> ravel::Task {
    link.send("one");
    link.send("two");
    co_return;
  });
  sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(10);  // Both are delivered by now.
    link.clear_inbox();
    after_clear = co_await link.receive_within(5);
  });
  CHECK(sim.run_until_quiescent().ok);
  CHECK(!after_clear.has_value());
}

TEST(clearing_the_inbox_under_a_pending_receive_fails_that_receive) {
  ravel::Simulation sim(1);
  auto& link = sim.add_channel("a", "b", {});
  sim.scheduler().spawn("sender", [&]() -> ravel::Task {
    link.send("one");
    co_return;
  });
  sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(10);
    co_await link.receive();  // A message is waiting: this task is queued to take it.
  });
  sim.scheduler().spawn("meddler", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(10);
    link.clear_inbox();
  });
  const ravel::Result result = sim.run_until_quiescent();
  // Depending on the order the two tasks run in, the clear either comes first
  // (the receive then blocks forever, which is fine) or second (it throws).
  CHECK(result.ok || result.failure.find("inbox was cleared") != std::string::npos);
}

TEST(a_time_limit_stops_a_system_that_never_goes_quiet) {
  ravel::Simulation sim(1, ravel::SimulationOptions{.time_limit = 100});
  int ticks = 0;
  ravel::VirtualClock::Tick last_tick = 0;
  sim.scheduler().spawn("heartbeat", [&]() -> ravel::Task {
    while (true) {
      co_await sim.scheduler().sleep(10);
      ++ticks;
      last_tick = sim.clock().now();
    }
  });
  const ravel::Result result = sim.run_until_quiescent();
  CHECK(result.ok);
  CHECK(ticks == 10);        // t = 10, 20, ... 100.
  CHECK(last_tick == 100);
}

TEST(invariants_are_checked_when_the_time_limit_ends_the_run) {
  ravel::Simulation sim(1, ravel::SimulationOptions{.time_limit = 50});
  int ticks = 0;
  sim.scheduler().spawn("heartbeat", [&]() -> ravel::Task {
    while (true) {
      co_await sim.scheduler().sleep(10);
      ++ticks;
    }
  });
  sim.add_invariant("beat_at_least_ten_times", [&ticks] { return ticks >= 10; });
  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.failure == "invariant 'beat_at_least_ten_times' failed");
}
