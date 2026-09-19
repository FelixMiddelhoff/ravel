# Making your code simulatable

ravel can only test code whose behavior depends on nothing but the seed. Real
code reads the clock, waits in real time, starts threads and talks to real
sockets and disks, and each of those makes a run unrepeatable. This guide shows
how to get your code into a shape ravel can test, one step at a time, with a
worked example.

- [The idea](#the-idea)
- [The five steps](#the-five-steps)
- [Worked example: a peer monitor](#worked-example-a-peer-monitor)
- [What to replace with what](#what-to-replace-with-what)
- [Is it really deterministic?](#is-it-really-deterministic)
- [Pitfalls with coroutines](#pitfalls-with-coroutines)
- [Checklist](#checklist)

## The idea

Your code does two kinds of things: it **decides** (what to send, when to give
up, which entry to keep) and it **touches the world** (reads the time, sends
bytes, writes a file). ravel replaces the world with a virtual one that it
controls, and leaves the decisions alone.

```
              real world                          under ravel

  your logic ── clock, sockets, disk, threads     your logic ── virtual clock, Channel, Disk, tasks
                                                                          ▲
                                                             one seeded scheduler decides
                                                             who runs, what is lost, what is late
```

The virtual world has one source of randomness, and everything in it (task
order, lost messages, delays, crashes) is drawn from that source. So a run is a
pure function of its seed.

The one thing this asks of you: **your logic must reach the world only through
things ravel can replace.** ravel does not intercept system calls (see the
[FAQ](../README.md#faq)); you make the seam yourself, once, and it pays off
in code that is easier to test in every other way too.

## The five steps

1. **List what your code reads from outside.** The clock, random numbers,
   network, disks, threads and timers. Search for `std::chrono`, `rand`,
   `std::random_device`, `sleep_for`, `std::thread`, sockets and file streams.
2. **Separate the logic from the I/O.** Move decisions into a class that takes
   its inputs as arguments and returns its outputs, with no clock, no socket, no
   thread inside. (The next section shows this.)
3. **Write a driver.** A few lines of glue that feed the logic time and messages
   from ravel, and carry its outputs back out. In production a second, equally
   thin driver does it with real sockets.
4. **State the invariants.** What must always be true? "Never more than one
   leader per term." "Acknowledged writes survive a crash." "The balance is never
   negative." Writing them down is most of the value.
5. **Check determinism** with `ravel::check_determinism`, then run thousands of
   seeds.

You do not have to port everything. Start with one component (a protocol, a
cache, a queue) and simulate that.

## Worked example: a peer monitor

Here is code that cannot be simulated. A monitor pings a peer every 100 ms and
declares it dead if no answer has come for 300 ms:

<!-- snippet: docs/snippets/porting_before.cpp#before -->
```cpp
#include <atomic>
#include <chrono>
#include <thread>

struct Socket {                 // Stand-in for a real network connection.
  void send_ping() {}
  bool wait_for_pong(std::chrono::milliseconds) { return true; }
};

class PeerMonitor {
 public:
  PeerMonitor(std::chrono::milliseconds ping_every, std::chrono::milliseconds dead_after)
      : ping_every_(ping_every), dead_after_(dead_after), last_pong_(std::chrono::steady_clock::now()) {}

  void start() { thread_ = std::thread([this] { loop(); }); }
  ~PeerMonitor() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  bool peer_alive() const { return alive_; }

 private:
  void loop() {
    while (!stop_) {
      socket_.send_ping();                                    // Real network.
      if (socket_.wait_for_pong(ping_every_)) {
        last_pong_ = std::chrono::steady_clock::now();        // Real clock.
      }
      if (std::chrono::steady_clock::now() - last_pong_ > dead_after_) alive_ = false;
      std::this_thread::sleep_for(ping_every_);               // Real waiting.
    }
  }

  Socket socket_;
  std::chrono::milliseconds ping_every_, dead_after_;
  std::chrono::steady_clock::time_point last_pong_;
  std::atomic<bool> stop_{false}, alive_{true};
  std::thread thread_;                                        // A real thread.
};
```

Every line marked in the comments is a problem: it reads the real clock, waits in
real time, runs on a real thread and talks to a real socket. Testing "what if
three pings in a row are lost?" means unplugging cables and waiting seconds per
try, and a failure could never be replayed.

The fix is to pull the *logic* out. Look at what the monitor actually decides:
*when to ping* and *whether the peer is alive*. Both only need to know "what time
is it?". So make time an argument:

<!-- snippet: docs/snippets/porting_after.cpp#core -->
```cpp
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
```

This class has no clock, no thread, no socket. It is a plain state machine, and
you can unit-test it by hand: call `ponged(100)`, ask `peer_alive(350)`. In
production, a driver calls it with `steady_clock` time and real sockets. Under
ravel, a driver calls it with virtual time and virtual channels:

<!-- snippet: docs/snippets/porting_after.cpp#driver -->
```cpp
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
```

<!-- output: porting_after -->
```text
500 seeds run, 0 failed
```

The peer answers for 500 ticks and then goes silent, over a network that loses
20% of messages and delays the rest. Across 500 seeds the monitor always notices
the dead peer by the end. Make `peer_alive` always return `true` and this
run will tell you within a second.

The pattern to take away is **"sans-I/O"**: logic in the middle, no I/O in it,
and thin drivers at the edges. It is the same shape as the Raft example
(`Node` handlers change state and queue messages; the run loop does the I/O).

## What to replace with what

| Your code does | Under ravel |
|---|---|
| `std::chrono::steady_clock::now()` | `sim.clock().now()` (virtual ticks), or take the time as an argument |
| `std::this_thread::sleep_for(d)` | `co_await sim.scheduler().sleep(d)` |
| a timer or timeout | `co_await channel.receive_within(d)`, or `sleep` |
| `rand()`, `std::mt19937`, `std::random_device` | `sim.rng().next_below(n)`, `next_between(a, b)`, `chance(p)` |
| a socket, RPC or message queue | a `Channel` (`send`, `co_await receive()`), with a `FaultSpec` for loss, delay and reordering |
| `fstream`, `write`, `fsync`, `rename` | a `Disk`: `write`, `read`, `sync`, `rename`, `sync_dir`; `crash()` for power loss |
| `std::thread` / a thread pool | `sim.scheduler().spawn("name", ...)`: cooperative tasks |
| `std::mutex` / `condition_variable` | usually nothing: tasks only switch at `co_await`. (The interesting races are between those points, which is what ravel explores.) |
| "the process crashes and restarts" | `disk.crash()`, then a task that reboots from what is on the disk (see the Raft example) |

Two things worth knowing:

- **Randomness must come from `sim.rng()`**, including your own workload
  generators. Every draw is recorded and shrinkable, so a random workload shrinks
  too. Anything random from elsewhere breaks reproducibility.
- **Yield points are where interleavings happen.** Two tasks that never `co_await`
  between two steps cannot be interleaved between them. If you want ravel to try
  reordering operations, give it a `co_await sim.scheduler().yield()` between them.

## Is it really deterministic?

ravel's promise, that a seed always gives the same run, only holds if nothing
else influences the code under test. The usual suspects:

| Leak | Why it breaks determinism |
|---|---|
| `std::chrono::...::now()`, `time()`, `clock()` | different on every run |
| `rand()`, `std::random_device`, address-derived seeds | different on every run |
| `std::thread`, `std::async`, thread pools | the OS decides the interleaving |
| statics and globals that survive between runs | the second run of a seed starts in a different state |
| `std::unordered_map` / `unordered_set` keyed by **pointers** | iteration order depends on addresses, which differ between runs |
| sorting or hashing by pointer value | same reason |
| uninitialized memory | whatever was there last |
| reading environment variables, files or the network | different on different machines |
| `%p`, `this`, addresses in logging that feed decisions | addresses differ |

You do not have to hunt for these by reading. Ask ravel:

```cpp
ravel::DeterminismReport report = ravel::check_determinism(my_setup);
```

It runs each seed twice (and once more replayed from the recorded choices) and
compares the runs event by event. Here it is on code with a leak (a static that
survives between runs):

<!-- snippet: docs/snippets/porting_determinism.cpp#leaky -->
```cpp
// A function-level static keeps its value from one run to the next, so the
// same seed does not behave the same way twice.
void leaky_setup(ravel::Simulation& sim) {
  static int runs_so_far = 0;
  const ravel::VirtualClock::Tick nap = (++runs_so_far % 2 == 0) ? 5 : 10;

  sim.scheduler().spawn("worker", [&sim, nap]() -> ravel::Task {
    co_await sim.scheduler().sleep(nap);
  });
}
```

<!-- snippet: docs/snippets/porting_determinism.cpp#check -->
```cpp
int main() {
  ravel::DeterminismOptions options;
  options.seed_count = 3;
  options.threads = 1;  // This setup shares a static, so run it on one thread.

  const ravel::DeterminismReport report = ravel::check_determinism(leaky_setup, options);
  std::printf("%llu seeds checked, %zu with problems\n",
              static_cast<unsigned long long>(report.seeds_checked), report.problems.size());
  for (const ravel::DeterminismProblem& problem : report.problems) {
    std::printf("seed %llu: %s\n", static_cast<unsigned long long>(problem.seed),
                problem.description.c_str());
  }
  return report.ok() ? 1 : 0;  // This example is supposed to find something.
}
```

<!-- output: porting_determinism -->
```text
3 seeds checked, 3 with problems
seed 0: two runs of the same seed diverged at step 2: run 1 TaskResumed 'worker' at t=10, run 2 TaskResumed 'worker' at t=5
seed 1: two runs of the same seed diverged at step 2: run 1 TaskResumed 'worker' at t=10, run 2 TaskResumed 'worker' at t=5
seed 2: two runs of the same seed diverged at step 2: run 1 TaskResumed 'worker' at t=10, run 2 TaskResumed 'worker' at t=5
```

The first message is the useful part: it names the **first step where the runs
part ways**. Here, step 2 is when the worker wakes up; run 1 slept until `t=10`
and run 2 until `t=5`. Something that feeds the sleep time differs between the
runs: look at where `nap` comes from and you find the static.

Make `check_determinism` part of your test suite, over a few dozen seeds. It is
cheap, and it turns "my simulation is flaky" from a mystery into a line number.
If you skip it, `ravel::shrink` still refuses to shrink a failure that does not
replay, but that is a late and vague warning.

## Pitfalls with coroutines

**1. Do not capture locals of the setup function.** The setup returns before the
tasks run, so a captured local is already gone:

```cpp
// WRONG: `count` is destroyed when the setup returns; the task reads garbage.
[](ravel::Simulation& sim) {
  int count = 0;
  sim.scheduler().spawn("t", [&]() -> ravel::Task { ++count; co_return; });
}

// RIGHT: make_state gives the value a home that outlives the tasks.
[](ravel::Simulation& sim) {
  int& count = sim.make_state<int>(0);
  sim.scheduler().spawn("t", [&count]() -> ravel::Task { ++count; co_return; });
}
```

Capture references to things made with `make_state`, or to channels and disks
from `add_channel`/`add_disk` (those live as long as the simulation). Capturing
by value is always fine.

**2. A task that simulates a process must stop when the power goes.** After
`disk.crash()`, an operation that completed a moment earlier still resumes its
task. Read `disk.crash_count()` when the process starts and stop when it changes;
the Raft example's `dead()` does this.

**3. Never start real threads or block.** A task that calls something that
really waits (a real `sleep`, a blocking read) stalls the whole simulation, and
one that starts a thread makes runs unrepeatable. ravel throws if a foreign
thread touches a running simulation, but it cannot catch everything.

**4. Do not keep state between runs.** Statics, globals and singletons survive from
one seed to the next. Keep per-run state in `make_state`. (`check_determinism`
catches the ones you miss.)

**5. Setup runs on several threads at once** in `run_seeds` and `shrink`, so it
must not touch shared mutable data. Read-only shared data is fine.

## Checklist

For a first port:

- [ ] The logic has no clock, thread, socket, or file inside; time and I/O come in
      through arguments or a driver.
- [ ] Randomness comes from `sim.rng()` only.
- [ ] Per-run state lives in `make_state`, not in captures or statics.
- [ ] The driver has `co_await`s (or `yield`s) between the steps you want reordered.
- [ ] There is at least one invariant, phrased as something that must always hold.
- [ ] `check_determinism` passes on a few dozen seeds.
- [ ] `run_seeds` runs thousands of seeds, with `shrink_first_failure` and a
      `trace_dir`, so a failure leaves its reproducer behind.
- [ ] The first failure you find is turned into a regression test (see the
      [tutorial](tutorial.md#8-keep-it-fixed)).

When something goes wrong along the way, [debugging.md](debugging.md) shows
what each failure looks like and what to do about it.
