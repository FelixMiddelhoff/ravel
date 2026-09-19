#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "ravel/network.hpp"
#include "ravel/simulation.hpp"
#include "testing.hpp"

namespace {

// Spawns a receiver that appends every message it gets to `received`. It
// loops forever, so it is still waiting when the run goes quiet; that is fine.
void spawn_collector(ravel::Simulation& sim, ravel::Channel& channel,
                     std::vector<std::string>& received) {
  sim.scheduler().spawn("collector", [&]() -> ravel::Task {
    while (true) received.push_back(co_await channel.receive());
  });
}

// Sends "0", "1", ... "count-1" back to back from one task.
void spawn_numbered_sender(ravel::Simulation& sim, ravel::Channel& channel, int count) {
  sim.scheduler().spawn("sender", [&, count]() -> ravel::Task {
    for (int i = 0; i < count; ++i) channel.send(std::to_string(i));
    co_return;
  });
}

std::vector<std::string> in_send_order(int count) {
  std::vector<std::string> expected;
  for (int i = 0; i < count; ++i) expected.push_back(std::to_string(i));
  return expected;
}

}  // namespace

TEST(channel_delivers_a_message_after_its_latency) {
  ravel::Simulation sim(1);
  auto& channel = sim.add_channel("a", "b", {.latency_min = 10, .latency_max = 10});

  ravel::VirtualClock::Tick received_at = 0;
  std::string payload;
  sim.scheduler().spawn("sender", [&]() -> ravel::Task {
    channel.send("hello");
    co_return;
  });
  sim.scheduler().spawn("receiver", [&]() -> ravel::Task {
    payload = co_await channel.receive();
    received_at = sim.clock().now();
  });

  CHECK(sim.run_until_quiescent().ok);
  CHECK(payload == "hello");
  CHECK(received_at == 10);
}

TEST(channel_receive_returns_a_message_that_arrived_earlier) {
  ravel::Simulation sim(1);
  auto& channel = sim.add_channel("a", "b", {});

  std::string payload;
  sim.scheduler().spawn("late_receiver", [&]() -> ravel::Task {
    co_await sim.scheduler().sleep(100);  // The message is long delivered by now.
    payload = co_await channel.receive();
  });
  sim.scheduler().spawn("sender", [&]() -> ravel::Task {
    channel.send("early");
    co_return;
  });

  CHECK(sim.run_until_quiescent().ok);
  CHECK(payload == "early");
}

TEST(channel_keeps_send_order_when_reordering_is_off) {
  for (std::uint64_t seed = 0; seed < 30; ++seed) {
    ravel::Simulation sim(seed);
    auto& channel = sim.add_channel("a", "b", {.latency_min = 1, .latency_max = 50});
    std::vector<std::string> received;
    spawn_collector(sim, channel, received);
    spawn_numbered_sender(sim, channel, 20);

    CHECK(sim.run_until_quiescent().ok);
    CHECK(received == in_send_order(20));
  }
}

TEST(channel_can_reorder_messages_when_allowed) {
  int reordered_runs = 0;
  for (std::uint64_t seed = 0; seed < 30; ++seed) {
    ravel::Simulation sim(seed);
    auto& channel = sim.add_channel(
        "a", "b", {.latency_min = 1, .latency_max = 50, .allow_reorder = true});
    std::vector<std::string> received;
    spawn_collector(sim, channel, received);
    spawn_numbered_sender(sim, channel, 20);

    CHECK(sim.run_until_quiescent().ok);
    CHECK(received.size() == 20);  // Reordering never loses messages.
    if (received != in_send_order(20)) ++reordered_runs;
  }
  CHECK(reordered_runs > 0);
}

TEST(channel_with_no_loss_delivers_everything) {
  ravel::Simulation sim(1);
  auto& channel = sim.add_channel("a", "b", {});
  std::vector<std::string> received;
  spawn_collector(sim, channel, received);
  spawn_numbered_sender(sim, channel, 100);

  CHECK(sim.run_until_quiescent().ok);
  CHECK(received.size() == 100);
}

TEST(channel_drops_some_but_not_all_messages_at_partial_loss) {
  ravel::Simulation sim(1);
  auto& channel = sim.add_channel("a", "b", {.loss_probability = 0.5});
  std::vector<std::string> received;
  spawn_collector(sim, channel, received);
  spawn_numbered_sender(sim, channel, 200);

  CHECK(sim.run_until_quiescent().ok);
  CHECK(received.size() > 50);
  CHECK(received.size() < 150);
}

TEST(channel_with_total_loss_delivers_nothing) {
  ravel::Simulation sim(1);
  auto& channel = sim.add_channel("a", "b", {.loss_probability = 1.0});
  std::vector<std::string> received;
  spawn_collector(sim, channel, received);
  spawn_numbered_sender(sim, channel, 50);

  CHECK(sim.run_until_quiescent().ok);
  CHECK(received.empty());
}

TEST(channel_faults_replay_exactly_for_a_seed) {
  const auto run = [](std::uint64_t seed) {
    ravel::Simulation sim(seed);
    auto& channel = sim.add_channel("a", "b",
                                    {.loss_probability = 0.3,
                                     .latency_min = 1,
                                     .latency_max = 40,
                                     .allow_reorder = true});
    std::vector<std::string> received;
    spawn_collector(sim, channel, received);
    spawn_numbered_sender(sim, channel, 50);
    return std::pair{sim.run_until_quiescent(), received};
  };

  for (std::uint64_t seed = 0; seed < 20; ++seed) {
    const auto [first, first_received] = run(seed);
    const auto [second, second_received] = run(seed);
    CHECK(first.trace_digest == second.trace_digest);
    CHECK(first_received == second_received);
  }
  CHECK(run(1).first.trace_digest != run(2).first.trace_digest);
}

TEST(channel_supports_a_request_reply_exchange) {
  ravel::Simulation sim(1);
  auto& requests = sim.add_channel("client", "server", {.latency_min = 5, .latency_max = 5});
  auto& replies = sim.add_channel("server", "client", {.latency_min = 7, .latency_max = 7});

  sim.scheduler().spawn("server", [&]() -> ravel::Task {
    while (true) replies.send("re:" + co_await requests.receive());
  });

  std::string reply;
  ravel::VirtualClock::Tick reply_at = 0;
  sim.scheduler().spawn("client", [&]() -> ravel::Task {
    requests.send("question");
    reply = co_await replies.receive();
    reply_at = sim.clock().now();
  });

  CHECK(sim.run_until_quiescent().ok);
  CHECK(reply == "re:question");
  CHECK(reply_at == 12);  // 5 ticks out, 7 ticks back.
}

TEST(channel_rejects_an_invalid_fault_spec) {
  ravel::Simulation sim(1);
  bool loss_rejected = false;
  bool latency_rejected = false;
  try {
    sim.add_channel("a", "b", {.loss_probability = 1.5});
  } catch (const std::invalid_argument&) {
    loss_rejected = true;
  }
  try {
    sim.add_channel("a", "b", {.latency_min = 9, .latency_max = 3});
  } catch (const std::invalid_argument&) {
    latency_rejected = true;
  }
  CHECK(loss_rejected);
  CHECK(latency_rejected);
}

TEST(channel_fails_the_run_when_two_tasks_receive_at_once) {
  ravel::Simulation sim(1);
  auto& channel = sim.add_channel("a", "b", {});
  for (const char* name : {"first", "second"}) {
    sim.scheduler().spawn(name, [&]() -> ravel::Task {
      co_await channel.receive();
    });
  }

  const ravel::Result result = sim.run_until_quiescent();
  CHECK(!result.ok);
  CHECK(result.failure.find("already waiting to receive") != std::string::npos);
}
