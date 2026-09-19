// After: the same monitor with its logic separated from time, threads and
// sockets, driven by ravel in a test.

// [core]
#include <cstdint>

// The logic, and nothing else. Time arrives as an argument (a plain number of
// ticks), and there is no I/O: it only answers questions and remembers things.
class PeerMonitor {
 public:
  using Tick = std::uint64_t;

  PeerMonitor(Tick ping_every, Tick dead_after) : ping_every_(ping_every), dead_after_(dead_after) {}

  bool should_ping(Tick now) const { return now >= next_ping_; }
  void pinged(Tick now) { next_ping_ = now + ping_every_; }
  void ponged(Tick now) { last_pong_ = now; }
  bool peer_alive(Tick now) const { return now - last_pong_ <= dead_after_; }

 private:
  Tick ping_every_;
  Tick dead_after_;
  Tick next_ping_ = 0;
  Tick last_pong_ = 0;
};
// [/core]

#include <cstdio>

#include "ravel/runner.hpp"

// [driver]
// The driver: what a real deployment does with sockets and a clock, done here
// with ravel's virtual ones. A second, thin driver would do it for real.
ravel::SimulationSetup monitor_setup() {
  return [](ravel::Simulation& sim) {
    PeerMonitor& monitor = sim.make_state<PeerMonitor>(/*ping_every=*/100, /*dead_after=*/300);

    const ravel::FaultSpec network{.loss_probability = 0.2, .latency_min = 1, .latency_max = 20};
    ravel::Channel& pings = sim.add_channel("monitor", "peer", network);
    ravel::Channel& pongs = sim.add_channel("peer", "monitor", network);

    // The peer answers pings for the first 500 ticks, then stops for good.
    sim.scheduler().spawn("peer", [&sim, &pings, &pongs]() -> ravel::Task {
      while (sim.clock().now() < 500) {
        if (co_await pings.receive_within(100)) pongs.send("pong");
      }
    });

    sim.scheduler().spawn("monitor", [&sim, &monitor, &pongs, &pings]() -> ravel::Task {
      while (true) {
        if (monitor.should_ping(sim.clock().now())) {
          pings.send("ping");
          monitor.pinged(sim.clock().now());
        }
        if (co_await pongs.receive_within(25)) monitor.ponged(sim.clock().now());
      }
    });

    // The run stops at 1500 ticks; by then the monitor must have noticed.
    sim.add_invariant("dead_peer_is_noticed",
                      [&sim, &monitor] { return !monitor.peer_alive(sim.clock().now()); });
  };
}

int main() {
  ravel::RunnerOptions options;
  options.seed_count = 500;
  options.simulation.time_limit = 1500;  // The monitor never stops on its own.

  const ravel::RunnerReport report = ravel::run_seeds(monitor_setup(), options);
  std::printf("%llu seeds run, %zu failed\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  return report.ok() ? 0 : 1;
}
// [/driver]
