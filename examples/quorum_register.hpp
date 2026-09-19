#pragma once

// A replicated register on three replicas, written and read through quorums.
//
// The client writes a value, waits until `write_quorum` replicas acknowledge,
// then reads from two replicas and takes the newest value it sees. The write
// is durable once acknowledged, so the read must return it.
//
// That only holds if the write and read quorums overlap: write_quorum +
// read_quorum > replicas. With write_quorum = 1 they can miss each other and
// the read returns a stale value; with write_quorum = 2 they cannot. It is
// the kind of bug that hides for months in production, because it needs one
// slow replica at the wrong moment.

#include <array>
#include <string>

#include "ravel/simulation.hpp"

namespace quorum_register {

constexpr int kReplicas = 3;
constexpr int kReadQuorum = 2;
constexpr int kWrittenValue = 42;

struct State {
  std::array<int, kReplicas> replica_version{};
  std::array<int, kReplicas> replica_value{};
  int read_value = -1;
  bool read_completed = false;
};

// Builds the system for one simulation. `write_quorum` 1 is buggy, 2 is correct.
inline ravel::SimulationSetup setup(int write_quorum) {
  return [write_quorum](ravel::Simulation& sim) {
    State& state = sim.make_state<State>();

    // Messages are delayed and may overtake each other; none are lost.
    const ravel::FaultSpec network{.latency_min = 1, .latency_max = 20, .allow_reorder = true};
    std::array<ravel::Channel*, kReplicas> to_replica{};
    for (int i = 0; i < kReplicas; ++i) {
      to_replica[i] = &sim.add_channel("client", "replica" + std::to_string(i), network);
    }
    ravel::Channel& acks = sim.add_channel("replicas", "client-acks", network);
    ravel::Channel& read_replies = sim.add_channel("replicas", "client-reads", network);

    for (int i = 0; i < kReplicas; ++i) {
      sim.scheduler().spawn("replica" + std::to_string(i), [&state, &acks, &read_replies,
                                                            inbox = to_replica[i], i]() -> ravel::Task {
        while (true) {
          const ravel::Message request = co_await inbox->receive();
          if (request == "write") {
            state.replica_version[i] = 1;
            state.replica_value[i] = kWrittenValue;
            acks.send("ack");
          } else {
            read_replies.send(std::to_string(state.replica_version[i]) + " " +
                              std::to_string(state.replica_value[i]));
          }
        }
      });
    }

    sim.scheduler().spawn("client", [&state, &acks, &read_replies, to_replica,
                                     write_quorum]() -> ravel::Task {
      for (ravel::Channel* replica : to_replica) replica->send("write");
      for (int acked = 0; acked < write_quorum; ++acked) co_await acks.receive();

      // The write is acknowledged. Now read it back.
      for (ravel::Channel* replica : to_replica) replica->send("read");
      int newest_version = -1;
      for (int replies = 0; replies < kReadQuorum; ++replies) {
        const ravel::Message reply = co_await read_replies.receive();
        const std::size_t space = reply.find(' ');
        const int version = std::stoi(reply.substr(0, space));
        if (version > newest_version) {
          newest_version = version;
          state.read_value = std::stoi(reply.substr(space + 1));
        }
      }
      state.read_completed = true;
    });

    sim.add_invariant("read_completed", [&state] { return state.read_completed; });
    sim.add_invariant("read_sees_acknowledged_write",
                      [&state] { return state.read_value == kWrittenValue; });
  };
}

}  // namespace quorum_register
