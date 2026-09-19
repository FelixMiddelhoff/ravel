// Small failing runs, so docs/debugging.md can show what ravel prints for each
// kind of problem.   debugging_examples <mode>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ravel/determinism.hpp"
#include "ravel/shrink.hpp"
#include "ravel/version.hpp"

namespace {

void print_result(const ravel::Result& result) {
  std::printf("ok: %s\nfailure: %s\nsteps: %llu\nseed: %llu\n", result.ok ? "yes" : "no",
              result.failure.empty() ? "(none)" : result.failure.c_str(),
              static_cast<unsigned long long>(result.steps),
              static_cast<unsigned long long>(result.seed));
}

// [livelock]
// Two tasks that keep yielding to each other and never finish.
void livelock() {
  ravel::Simulation sim(1, ravel::SimulationOptions{.max_steps = 200});
  for (const char* name : {"ping", "pong"}) {
    sim.scheduler().spawn(name, [&sim]() -> ravel::Task {
      while (true) co_await sim.scheduler().yield();
    });
  }
  print_result(sim.run_until_quiescent());
}
// [/livelock]

// [task_threw]
// A task that hits a bug and lets an exception escape.
void task_threw() {
  ravel::Simulation sim(1);
  sim.scheduler().spawn("account_service", []() -> ravel::Task {
    throw std::runtime_error("no such account: 42");
    co_return;
  });
  print_result(sim.run_until_quiescent());
}
// [/task_threw]

// [stuck]
// The run "completes", because nothing is left to happen, yet nothing useful
// did: the client waits for a reply from a server that was never started.
void stuck() {
  ravel::Simulation sim(1);
  bool got_reply = false;
  ravel::Channel& to_server = sim.add_channel("client", "server", {});
  ravel::Channel& to_client = sim.add_channel("server", "client", {});
  sim.scheduler().spawn("client", [&]() -> ravel::Task {
    to_server.send("hello");
    co_await to_client.receive();  // Blocks forever.
    got_reply = true;
  });
  sim.add_invariant("client_got_a_reply", [&got_reply] { return got_reply; });
  print_result(sim.run_until_quiescent());
}
// [/stuck]

// [not_reproducible]
// Asking to shrink a failure that does not happen the same way twice.
void not_reproducible() {
  int runs = 0;
  const auto flaky = [&runs](ravel::Simulation& sim) {
    const bool fail = ++runs == 1;  // Fails the first time only.
    sim.add_invariant("flaky", [fail] { return !fail; });
  };
  try {
    ravel::shrink(flaky, /*seed=*/7);
  } catch (const std::exception& e) {
    std::printf("shrink threw: %s\n", e.what());
  }
}
// [/not_reproducible]

// [trace]
// A message, then a disk write and sync: the whole trace, one event per line.
void trace() {
  ravel::Simulation sim(3);
  ravel::Channel& link = sim.add_channel("client", "server", {.latency_min = 2, .latency_max = 5});
  ravel::Disk& disk = sim.add_disk("ssd", {.latency_min = 1, .latency_max = 3});
  sim.scheduler().spawn("client", [&]() -> ravel::Task {
    link.send("hello");
    co_await disk.write("log", 0, "hello");
    co_await disk.sync("log");
  });
  sim.scheduler().spawn("server", [&]() -> ravel::Task { co_await link.receive(); });
  sim.run_until_quiescent();

  std::ostringstream text;
  sim.write_trace(text);
  std::string trace_text = text.str();
  const std::string version = ravel::version_string();
  trace_text.replace(trace_text.find(version), version.size(), "x.y.z");  // Keep the docs stable.
  std::fputs(trace_text.c_str(), stdout);
}
// [/trace]

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "livelock") return livelock(), 0;
  if (mode == "task-threw") return task_threw(), 0;
  if (mode == "stuck") return stuck(), 0;
  if (mode == "not-reproducible") return not_reproducible(), 0;
  if (mode == "trace") return trace(), 0;

  std::puts("usage: debugging_examples livelock|task-threw|stuck|not-reproducible|trace");
  return 0;
}
