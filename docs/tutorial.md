# Tutorial: your first ravel test

In about half an hour you will find a real bug, watch ravel shrink it to a
handful of steps, read exactly what happened, fix it, and leave behind a test
that guards against it forever.

The bug is a classic. A client asks a server to deposit money and **retries if
it hears nothing back**. If the server's reply is lost, the retry makes the
server deposit twice. It hides in production for months, because it needs a
network fault at the wrong moment. ravel finds it in a fraction of a second.

Everything on this page is real: the code is compiled and the output is
produced by the actual programs (see `tools/check_docs.py`). You can find
them in [`docs/snippets`](snippets).

- [1. Install](#1-install)
- [2. A first simulation](#2-a-first-simulation)
- [3. The system to test](#3-the-system-to-test)
- [4. Hunt for the bug](#4-hunt-for-the-bug)
- [5. Read what happened](#5-read-what-happened)
- [6. What is a "choice"?](#6-what-is-a-choice)
- [7. Fix it](#7-fix-it)
- [8. Keep it fixed](#8-keep-it-fixed)
- [9. Run it in CI](#9-run-it-in-ci)
- [Where next](#where-next)

## 1. Install

ravel needs a C++20 compiler with coroutines (GCC 10+, Clang 14+, MSVC 2019+)
and CMake 3.20+. The simplest way in is CMake's `FetchContent`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_tests LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(ravel
  GIT_REPOSITORY https://github.com/FelixMiddelhoff/ravel.git
  GIT_TAG        v0.3.0)
FetchContent_MakeAvailable(ravel)

add_executable(my_test my_test.cpp)
target_link_libraries(my_test PRIVATE ravel::ravel)
target_compile_features(my_test PRIVATE cxx_std_20)
```

Prefer a package manager? The repository carries a
[vcpkg port](../packaging/vcpkg/ports/ravel) (use it with
`--overlay-ports=packaging/vcpkg/ports`) and a
[Conan recipe](../packaging/conan) (`conan create packaging/conan`). After
either, `find_package(ravel CONFIG REQUIRED)` gives you the same
`ravel::ravel` target.

## 2. A first simulation

Start with the smallest thing that works, to see the moving parts:

<!-- snippet: docs/snippets/tutorial_first_test.cpp#first_test -->
```cpp
#include <cstdio>

#include "ravel/runner.hpp"

int main() {
  ravel::RunnerOptions options;
  options.seed_count = 100;

  const ravel::RunnerReport report = ravel::run_seeds(
      [](ravel::Simulation& sim) {
        // One task that sleeps for ten virtual ticks, and a check that always passes.
        sim.scheduler().spawn("hello", [&sim]() -> ravel::Task {
          co_await sim.scheduler().sleep(10);
        });
        sim.add_invariant("always_true", [] { return true; });
      },
      options);

  std::printf("%llu seeds run, %zu failed\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  return report.ok() ? 0 : 1;
}
```

Build and run it:

<!-- output: tutorial_first_test -->
```text
100 seeds run, 0 failed
```

What just happened:

- A **simulation** is one complete run of your system under one **seed**. It
  owns everything the code under test may use: a virtual clock, a random source,
  a scheduler, and (later) a network and disks.
- **Tasks** are C++20 coroutines. `co_await sim.scheduler().sleep(10)` waits for
  ten *virtual* ticks. Nothing really waits: when every task is asleep, ravel
  jumps the clock forward. A simulated hour costs microseconds.
- An **invariant** is a property that must hold when the run ends.
- `run_seeds` runs the same setup under 100 different seeds. Each seed makes
  different random choices (which task goes next, which message is lost), so
  each explores a different behavior of your system. **Same seed, same run,
  every time, on every machine.** That is what makes failures reproducible.

## 3. The system to test

Now the real system. It is short enough to read in one go:

<!-- snippet: docs/snippets/deposit.hpp#deposit_system -->
```cpp
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
```

The pieces worth noticing:

- **`SimulationSetup`.** A function that builds one run from scratch: it adds
  the network, spawns the tasks and registers the invariants. ravel calls it once
  per seed, on a fresh `Simulation`.
- **`sim.make_state<Bank>()`.** State shared by tasks and invariants. Use it
  instead of locals: it is created fresh for each run and lives as long as the
  tasks do. (The setup function returns before the tasks run, so a local
  variable would already be gone. See the
  [porting guide](porting.md#pitfalls-with-coroutines).)
- **`add_channel(...)`.** A one-way virtual network link. The `FaultSpec` makes it
  lose 30% of messages and delay the rest. Faults are random *choices* like any
  other, so they are reproducible.
- **`receive_within(50)`.** Wait for a message, but give up after 50 ticks. That
  is the client's retry timer.
- **The server has no deduplication when `dedupe` is false.** That is the bug we
  are about to find. It is not obvious from reading the code, which is the point.

## 4. Hunt for the bug

Run the system under a thousand seeds. Ask ravel to shrink the first failure
and save what happened:

<!-- snippet: docs/snippets/tutorial_deposit.cpp#deposit_main -->
```cpp
int main(int argc, char** argv) {
  ravel::RunnerOptions options;
  options.seed_count = 1000;
  options.shrink_first_failure = true;              // Minimize the first failure...
  options.simulation.trace_dir = "ravel-traces";    // ...and save what happened.

  const ravel::RunnerReport report = ravel::run_seeds(deposit_setup(/*dedupe=*/false), options);
  if (report.ok()) return 0;
  const ravel::ShrinkResult& shrunk = *report.shrunk;
  if (print_saved_file(argc > 1 ? argv[1] : "", shrunk)) return 0;

  std::printf("%llu seeds run, %zu failed\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  const ravel::Result& first = report.failures.front();
  std::printf("first failure: seed %llu: %s\n", static_cast<unsigned long long>(first.seed),
              first.failure.c_str());
  std::printf("shrunk from %zu random choices (%llu steps) to %zu (%llu steps)\n",
              shrunk.original_choices.size(), static_cast<unsigned long long>(shrunk.original.steps),
              shrunk.choices.size(), static_cast<unsigned long long>(shrunk.minimal.steps));
  std::printf("minimal choices:");
  for (const auto choice : shrunk.choices) std::printf(" %llu", static_cast<unsigned long long>(choice));
  std::printf("\nsaved: %s\n", forward_slashes(shrunk.choices_path).c_str());
  std::printf("trace: %s\n", forward_slashes(shrunk.minimal.trace_path).c_str());
  return 0;
}
```

<!-- output: tutorial_deposit -->
```text
1000 seeds run, 281 failed
first failure: seed 6: invariant 'deposit_applied_at_most_once' failed
shrunk from 11 random choices (9 steps) to 4 (6 steps)
minimal choices: 0 0 0 1
saved: ravel-traces/ravel-seed-6.choices
trace: ravel-traces/ravel-seed-6.replay.trace.jsonl
```

(`print_saved_file` only does something when the program is run with `--trace`
or `--choices-file`, as we will in a moment: it prints that saved file instead of
the summary.)

Line by line:

- **`1000 seeds run, 281 failed`.** More than a quarter of seeds expose the bug.
  That is typical: bugs that need a fault at the wrong moment show up on a
  fraction of seeds, and a thousand seeds run in well under a second.
- **`first failure: seed 6`.** Seed 6 is the lowest failing seed. Run it again and
  you get exactly the same failure, today, tomorrow, on a colleague's laptop.
  The message names the invariant that broke.
- **`shrunk from 11 random choices (9 steps) to 4 (6 steps)`.** The failing run
  had 11 random decisions. ravel replayed edited versions of them, keeping any
  edit that still failed the same way, until it could not simplify further:
  4 decisions, the smallest run that still has the bug.
- **`minimal choices: 0 0 0 1`.** Those 4 decisions. Section 6 explains how to
  read them.
- **`saved:` and `trace:`.** Two files, written because we set `trace_dir`. The
  first is a reproducer you can check in. The second is the story of the run.

## 5. Read what happened

The trace file has one line per event. Run the same program with `--trace` to
print it:

<!-- output: tutorial_deposit --trace -->
```text
{"format":"ravel-trace","trace_version":1,"ravel_version":"x.y.z","seed":6}
{"step":0,"time":0,"kind":"TaskSpawned","id":0,"name":"server"}
{"step":1,"time":0,"kind":"TaskSpawned","id":1,"name":"client"}
{"step":2,"time":0,"kind":"TaskResumed","id":0,"name":"server"}
{"step":3,"time":0,"kind":"TaskResumed","id":1,"name":"client"}
{"step":4,"time":0,"kind":"MessageSent","id":0,"name":"client->server"}
{"step":5,"time":1,"kind":"MessageDelivered","id":0,"name":"client->server"}
{"step":6,"time":1,"kind":"TaskResumed","id":0,"name":"server"}
{"step":7,"time":1,"kind":"MessageSent","id":1,"name":"server->client"}
{"step":8,"time":1,"kind":"MessageDropped","id":1,"name":"server->client"}
{"step":9,"time":50,"kind":"TaskResumed","id":1,"name":"client"}
{"step":10,"time":50,"kind":"MessageSent","id":0,"name":"client->server"}
{"step":11,"time":51,"kind":"MessageDelivered","id":0,"name":"client->server"}
{"step":12,"time":51,"kind":"TaskResumed","id":0,"name":"server"}
{"step":13,"time":51,"kind":"MessageSent","id":1,"name":"server->client"}
{"step":14,"time":52,"kind":"MessageDelivered","id":1,"name":"server->client"}
{"step":15,"time":52,"kind":"TaskResumed","id":1,"name":"client"}
{"step":16,"time":52,"kind":"TaskFinished","id":1,"name":"client"}
```

You can read this like a log. The important part:

```
step 4   t=0    MessageSent       client->server      the client asks for a deposit
step 5   t=1    MessageDelivered  client->server      the server gets it
step 6   t=1    TaskResumed       server              ...and deposits 10
step 7   t=1    MessageSent       server->client      the server replies "ok"
step 8   t=1    MessageDropped    server->client      the reply is lost
step 9   t=50   TaskResumed       client              the client waits 50 ticks, gets nothing
step 10  t=50   MessageSent       client->server      ...and asks again
step 11  t=51   MessageDelivered  client->server      the server gets the second request
step 12  t=51   TaskResumed       server              ...and deposits 10 again: balance 20
```

There it is. The reply was lost at step 8, so the client retried, and the server
applied the same request twice. Every line of the trace is in virtual time (`t=`
is ticks, never wall-clock), so it reads the same however fast your machine is.

Prefer a picture? `tools/ravel_trace.py timeline` lays the same trace out with one
column per task and channel, like a sequence diagram, and `jq` slices it from the
command line; see [debugging.md](debugging.md#reading-traces-with-ravel_trace) for
both.

## 6. What is a "choice"?

Everything random in a run (which task goes next, whether a message is lost, how
long a delay is) is drawn from one source, and every draw is **recorded as a
small number where 0 means the simplest outcome**. The list of those numbers
describes the whole run. The saved file is exactly that list:

<!-- output: tutorial_deposit --choices-file -->
```text
ravel-choices 1
4
0 0 0 1
```

Our four numbers, in the order the draws happened:

| Choice | Draw | 0 means | Value | So |
|---|---|---|---|---|
| 1st | which task runs first | the one waiting longest | 0 | the server |
| 2nd | is the request lost? | no | 0 | it is delivered |
| 3rd | how long is its delay? | the minimum (1 tick) | 0 | 1 tick |
| 4th | is the reply lost? | no | **1** | **the reply is lost** |

Everything after those four draws takes the default, 0. So this is the minimal
recipe for the bug: *deliver the request, lose the reply.*

That is how shrinking works: it edits the list (drop some numbers, lower others),
replays it, and keeps the edit if the bug is still there. A shorter list with
smaller numbers is a simpler run. And because a run *is* its list, you can save
it, check it in, and replay it any time, with no seed and no dependence on how
ravel generates random numbers.

## 7. Fix it

The server must recognize a request it has already handled. That is one flag
in our setup (`dedupe`), which makes the server remember request ids:

<!-- snippet: docs/snippets/tutorial_deposit_fixed.cpp#deposit_fixed_main -->
```cpp
int main() {
  ravel::RunnerOptions options;
  options.seed_count = 1000;

  const ravel::RunnerReport report = ravel::run_seeds(deposit_setup(/*dedupe=*/true), options);
  std::printf("%llu seeds run, %zu failed\n", static_cast<unsigned long long>(report.seeds_run),
              report.failures.size());
  return report.ok() ? 0 : 1;
}
```

<!-- output: tutorial_deposit_fixed -->
```text
1000 seeds run, 0 failed
```

A thousand seeds, none failing. (More seeds are cheap: change `1000` to
`100000` and go for a coffee.)

## 8. Keep it fixed

Finding the bug once is not enough; you want a test that fails if it ever comes
back. The reproducer from step 4 is perfect for that. Paste its numbers into a
test and replay them against your code:

<!-- snippet: docs/snippets/tutorial_regression.cpp#regression -->
```cpp
// Found by ravel, shrunk to this, and checked in. No seed needed.
const ravel::Choices kDoubleDeposit = {0, 0, 0, 1};

int main() {
  const ravel::Result fixed = ravel::replay(deposit_setup(/*dedupe=*/true), kDoubleDeposit);
  const ravel::Result buggy = ravel::replay(deposit_setup(/*dedupe=*/false), kDoubleDeposit);

  std::printf("fixed server: %s\n", fixed.ok ? "passes" : "FAILS");
  std::printf("buggy server: %s\n", buggy.ok ? "passes" : ("fails: " + buggy.failure).c_str());
  return fixed.ok && !buggy.ok ? 0 : 1;
}
```

<!-- output: tutorial_regression -->
```text
fixed server: passes
buggy server: fails: invariant 'deposit_applied_at_most_once' failed
```

The fixed server passes; the buggy one fails on exactly this run, without needing
to search for a lucky seed. In a real project this goes in your normal unit
tests (gtest, Catch2, doctest, plain `main`: whatever you use). Keep the
sweep over many seeds too: the sweep finds *new* bugs, the reproducer stops
*old* ones from returning.

You can also keep the `.choices` file in your repository and load it with
`ravel::read_choices`. The text format is documented in
[formats.md](formats.md).

## 9. Run it in CI

A sweep is just a program that exits non-zero when a seed fails, so it drops into
any CI system. On GitHub Actions:

```yaml
- name: Simulation tests
  run: |
    cmake --build build --target my_test
    ./build/my_test
- name: Keep the evidence when it fails
  if: failure()
  uses: actions/upload-artifact@v4
  with:
    name: ravel-traces
    path: ravel-traces/
```

Set `options.simulation.trace_dir` as in step 4 and a failing run leaves its
trace and its reproducer in `ravel-traces/`; the artifact step keeps them so you
can download them and read the story of the failure.

Some habits that pay off:

- Run **many seeds in CI** (thousands) and a **few in local builds**.
- Run a **bigger sweep nightly** with a different `first_seed` each time. New
  seeds are new chances to find something.
- When a nightly run fails, **commit the shrunk `.choices` file** with the fix.

## Where next

- **[Making your code simulatable](porting.md)**: how to get *your* code into a
  shape ravel can test, with a before/after example.
- **[Debugging guide](debugging.md)**: what ravel prints for each kind of
  problem, and how to read traces.
- **[examples/raft.hpp](../examples/raft.hpp)**: a full Raft implementation
  with crashing nodes and disks, and three bugs ravel finds.
- **[File formats](formats.md)**: the trace and choices files, for tooling.
