# ravel

Deterministic simulation testing (DST) for distributed C++ systems — seed a
bug, replay it exact.

Status: **pre-alpha**. Requires C++20 (coroutines). Not ready for real use;
see the table below for what works today.

## What this is

FoundationDB/TigerBeetle-style deterministic simulation testing, for C++.
Virtualize time, RNG, and network behind seams a `Simulation` owns; run
under a controlled, seed-driven scheduler; on failure, the seed alone
reproduces the exact same interleaving and faults.

Rust has this (`turmoil`, `madsim`, `loom`). C++ doesn't have a portable,
permissively-licensed equivalent.

```cpp
ravel::RunnerReport report = ravel::run_seeds([](ravel::Simulation& sim) {
  int& counter = sim.make_state<int>(0);
  for (int i = 0; i < 3; ++i) {
    sim.scheduler().spawn("incrementer", [&sim, &counter]() -> ravel::Task {
      const int seen = counter;
      co_await sim.scheduler().yield();  // the scheduler may run any task here
      counter = seen + 1;                // lost update if one did
    });
  }
  sim.add_invariant("no_lost_updates", [&counter] { return counter == 3; });
});

for (const ravel::Result& r : report.failures)
  std::printf("seed %llu: %s\n", r.seed, r.failure.c_str());
```

`run_seeds` runs many seeds in parallel; some interleavings expose the bug,
and a failing seed replays identically. See [`examples/quickstart.cpp`](examples/quickstart.cpp).

When a seed fails, `shrink` boils the run down. Every random decision
(scheduling, message loss, delays, and anything your workload draws from
`sim.rng()`) is recorded as a small integer where 0 is the simplest outcome.
Shrinking edits that list, replays it, and keeps any change that still fails
the same way with a shorter or smaller list. A 40-message run with a lost
message shrinks to one that loses exactly one.

```cpp
ravel::RunnerOptions options;
options.shrink_first_failure = true;
options.simulation.trace_dir = "ravel-traces";  // saves traces and *.choices
auto report = ravel::run_seeds(setup, options);

// Later, as a permanent regression test (no seed needed):
std::ifstream file("ravel-traces/ravel-seed-4.choices");
ravel::Result r = ravel::replay(setup, ravel::read_choices(file));
```

Messages travel over virtual channels whose faults come from the same seed:

Disks model what makes storage code hard: a write is visible at once but only
durable after `sync`, and a crash loses, tears or keeps each unsynced write.

```cpp
auto& disk = sim.add_disk("ssd", {.latency_min = 1, .latency_max = 10});
co_await disk.write("wal", 0, "commit #1");
co_await disk.sync("wal");       // without this, disk.crash() may lose the commit
```

```cpp
auto& link = sim.add_channel("client", "server", {.loss_probability = 0.05,
                                                   .latency_min = 10, .latency_max = 50});
link.send("ping");                             // may be dropped or delayed
ravel::Message m = co_await link.receive();    // inside a task
```

Tasks are C++20 coroutines. At every `co_await scheduler.yield()` or
`co_await scheduler.sleep(ticks)` the seeded scheduler picks which runnable
task goes next. Time is virtual: when every task sleeps, the clock jumps
to the earliest wake-up.

## Status of the seams

| Seam | Today | Planned |
|---|---|---|
| `VirtualClock` / `VirtualRng` | done, seed-deterministic | - |
| `Scheduler` | seed-driven interleaving, virtual-time sleep | - |
| `Trace` | event log with a replay digest; JSON Lines dump of failed runs (`SimulationOptions::trace_dir`) | - |
| `Channel` / `FaultSpec` | one-way message channel with loss, latency, optional reordering | - |
| Multi-seed runner (`run_seeds`) | parallel over seeds, thread-count-independent report | - |
| Shrinking (`shrink`, `replay`) | minimizes a failing run; saves a replayable choices file | - |
| `Disk` / `DiskFaultSpec` | virtual files with write-back cache, `sync`, torn writes and lost writes on `crash()`, ENOSPC, I/O errors, latency | - |

## Building

```bash
cmake -S . -B build -DRAVEL_SHARED=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## C ABI

`include/ravel/ravel.h` exposes a minimal opaque-handle C surface for FFI
from languages/runtimes that can't link C++ directly. Stability policy:
[docs/abi-policy.md](docs/abi-policy.md).

## License

MIT — see [LICENSE](LICENSE).

## Security

See [SECURITY.md](SECURITY.md) for scope and misuse boundaries.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). This project follows the
[Contributor Covenant](CODE_OF_CONDUCT.md).
