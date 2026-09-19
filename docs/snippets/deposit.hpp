#pragma once

// The system the tutorial tests: a client asks a server to deposit 10, and
// retries if it hears nothing back. The network loses and delays messages.
// Without deduplication a retry can be applied twice.

// [deposit_system]
#include <set>
#include <string>

#include "ravel/simulation.hpp"

struct Bank {
  int balance = 0;
  bool client_got_ack = false;
  std::set<int> seen_requests;  // Request ids the server has already applied.
};

inline ravel::SimulationSetup deposit_setup(bool dedupe) {
  return [dedupe](ravel::Simulation& sim) {
    Bank& bank = sim.make_state<Bank>();  // Fresh for every run, and outlives the tasks.

    // Both directions lose 30% of messages and delay the rest by 1 to 10 ticks.
    const ravel::FaultSpec network{.loss_probability = 0.3, .latency_min = 1, .latency_max = 10};
    ravel::Channel& to_server = sim.add_channel("client", "server", network);
    ravel::Channel& to_client = sim.add_channel("server", "client", network);

    sim.scheduler().spawn("server", [&bank, &to_server, &to_client, dedupe]() -> ravel::Task {
      while (true) {
        const ravel::Message request = co_await to_server.receive();  // "deposit <id>"
        const int id = std::stoi(request.substr(8));
        if (!dedupe || bank.seen_requests.insert(id).second) bank.balance += 10;
        to_client.send("ok");
      }
    });

    sim.scheduler().spawn("client", [&bank, &to_server, &to_client]() -> ravel::Task {
      for (int attempt = 0; attempt < 5; ++attempt) {
        to_server.send("deposit 1");
        if (co_await to_client.receive_within(50)) {  // Wait up to 50 ticks for the "ok".
          bank.client_got_ack = true;
          co_return;
        }
      }
    });

    // Whatever happens to messages, money must not be created...
    sim.add_invariant("deposit_applied_at_most_once", [&bank] { return bank.balance <= 10; });
    // ...and a client that was told "ok" must find the money there.
    sim.add_invariant("acknowledged_deposit_applied",
                      [&bank] { return !bank.client_got_ack || bank.balance == 10; });
  };
}
// [/deposit_system]
