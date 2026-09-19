# ravel

[![ci](https://github.com/FelixMiddelhoff/ravel/actions/workflows/ci.yml/badge.svg)](https://github.com/FelixMiddelhoff/ravel/actions/workflows/ci.yml)
[![codeql](https://github.com/FelixMiddelhoff/ravel/actions/workflows/codeql.yml/badge.svg)](https://github.com/FelixMiddelhoff/ravel/actions/workflows/codeql.yml)
[![docs](https://github.com/FelixMiddelhoff/ravel/actions/workflows/docs.yml/badge.svg)](https://github.com/FelixMiddelhoff/ravel/actions/workflows/docs.yml)
[![packaging](https://github.com/FelixMiddelhoff/ravel/actions/workflows/packaging.yml/badge.svg)](https://github.com/FelixMiddelhoff/ravel/actions/workflows/packaging.yml)
[![release](https://img.shields.io/github/v/release/FelixMiddelhoff/ravel?include_prereleases)](https://github.com/FelixMiddelhoff/ravel/releases)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Deterministic simulation testing (DST) for distributed C++ systems — seed a
bug, replay it exact.

Status: **pre-1.0 (0.3.0)**. The features below work and are tested on Linux,
macOS and Windows, including under ASan, UBSan and TSan. It has not been used
outside this repository yet, so the API, the meaning of a seed and the C ABI
may still change between minor versions. Requires C++20 (coroutines).

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
message shrinks to one that loses exactly one. Candidates are replayed on
all hardware threads, and the result is the same whatever the thread count.
Besides dropping and lowering single choices, it moves value between two
draws that must add up to something (say, two delays) and lowers two draws
together.

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

```cpp
auto& link = sim.add_channel("client", "server", {.loss_probability = 0.05,
                                                   .latency_min = 10, .latency_max = 50});
link.send("ping");                             // may be dropped or delayed
ravel::Message m = co_await link.receive();    // inside a task
auto maybe = co_await link.receive_within(100); // gives up after 100 ticks
```

Systems that never go quiet on their own (heartbeats, election timers) stop at
`SimulationOptions::time_limit`, and their invariants are checked then.

Disks model what makes storage code hard: a write is visible at once but only
durable after `sync`, and a crash loses, tears or keeps each unsynced write.

```cpp
auto& disk = sim.add_disk("ssd", {.latency_min = 1, .latency_max = 10});
co_await disk.write("wal/000", 0, "commit #1");
co_await disk.sync("wal/000");   // without this, disk.crash() may lose the commit
co_await disk.rename("conf.tmp", "conf");
co_await disk.sync_dir("");      // a rename is only durable after this
```

Tasks are C++20 coroutines. At every `co_await scheduler.yield()` or
`co_await scheduler.sleep(ticks)` the seeded scheduler picks which runnable
task goes next. Time is virtual: when every task sleeps, the clock jumps
to the earliest wake-up.

## Showcase: Raft

[`examples/raft.hpp`](examples/raft.hpp) is a working Raft implementation
(leader election and log replication, each node persisting its state to a
virtual disk) that runs under ravel with nodes losing power at random, and a
lossy, reordering network. The correct version survives every seed. Three
deliberate bugs, of the kind real implementations have shipped with, are each
found by ravel:

| Bug | What breaks | Seeds that fail (of 600) |
|---|---|---|
| state file renamed into place before its data is synced | a rebooted node forgets entries it acknowledged | ~200 |
| rename never made durable (no `sync_dir`) | the same, but only if power fails right after a state change | ~1 |
| votes for candidates with stale logs | a new leader lacks committed entries | ~20 |

`ravel_raft` runs all four and shrinks the first bug from about 2000 random
choices to a few hundred, printing what the checkers saw (a leader without an
entry a node had already applied). The checkers are election safety, state
machine safety and leader completeness.

## Status of the seams

| Seam | Today | Planned |
|---|---|---|
| `VirtualClock` / `VirtualRng` | done, seed-deterministic | - |
| `Scheduler` | seed-driven interleaving, virtual-time sleep | - |
| `Trace` | event log with a replay digest; JSON Lines dump of failed runs (`SimulationOptions::trace_dir`) | - |
| `Channel` / `FaultSpec` | one-way message channel with loss, latency, optional reordering | - |
| Multi-seed runner (`run_seeds`) | parallel over seeds, thread-count-independent report | - |
| Shrinking (`shrink`, `replay`) | minimizes a failing run; saves a replayable choices file | - |
| `Disk` / `DiskFaultSpec` | virtual files and directories: write-back cache, `sync`, `rename`, `remove`, `sync_dir`, `list`; `crash()` loses, tears or keeps unsynced data and directory changes; ENOSPC, I/O errors, latency | - |

## Building

```bash
cmake -S . -B build -DRAVEL_SHARED=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## FAQ

**Do I have to change my code?** Yes, a little, and that is the trade. Code
under test takes its time, randomness, network and disk from ravel
(`sim.clock()`, `sim.rng()`, `Channel`, `Disk`) instead of calling
`std::chrono`, `rand()`, sockets or files directly, and runs its concurrency
as ravel tasks. ravel does not intercept syscalls or processes the way `rr`,
Antithesis or `LD_PRELOAD` tools do. What you give up is "works on a binary
you cannot modify". What you get is no ptrace, no root, no platform-specific
magic, identical behavior on Linux, macOS and Windows, and a run you can step
through in a normal debugger.

**Is a seed stable across ravel versions?** Not before 1.0. A change to how
randomness is consumed changes what every seed does. That is why a shrunk
failure is saved as a *choices file* (`*.choices`): a plain list of numbers
that replays the same failure and can be checked in as a regression test.
From 1.0, a seed will keep its meaning within a minor version line.

**Can my code use threads?** No. A simulation is single-threaded and
cooperative: tasks are coroutines, and ravel decides the order they run in.
Code that starts real threads, or reads a real clock or a real random source,
makes runs unrepeatable, and `shrink` will refuse it (it checks that replaying
a failure really reproduces it). `run_seeds` is parallel, but only across
independent simulations that share nothing.

**Is `VirtualRng` secure?** No. It is a fast, portable, reproducible PRNG for
simulation. Never use it for keys, nonces or tokens.

**Which compilers?** C++20 with coroutines: GCC 10+, Clang 14+, MSVC 2019+.

## Non-goals

- No syscall, binary or process interception.
- No real network or disk access: `Channel` and `Disk` are virtual, and the
  only files ravel writes are the traces and choice lists you ask for.
- No multithreaded code under test.
- No cryptographic randomness.
- No promise to find bugs outside the faults you model.

## Documentation

- **New here? Start with the [tutorial](docs/tutorial.md)**: find, shrink, read
  and fix a real bug in half an hour.
- [docs/concepts.md](docs/concepts.md): a one-page mental model, and a glossary.
- [docs/porting.md](docs/porting.md): making your own code simulatable, with a
  before/after example and a determinism checklist.
- [docs/debugging.md](docs/debugging.md): what ravel prints for each kind of
  failure, and how to read traces (with a `ravel_trace.py` viewer and differ).
- [docs/comparison.md](docs/comparison.md): how ravel relates to FoundationDB's
  simulator, turmoil, madsim, Antithesis, rr, Jepsen and property-based testing,
  and when another tool is the better choice.
- [docs/troubleshooting.md](docs/troubleshooting.md): every message ravel prints
  and what to do about it, plus common symptoms (build errors, crashes, "the
  sweep passes but there is a bug").
- [docs/ci.md](docs/ci.md): running ravel in CI: a ready-made command line
  (`run_sweep`), pull-request and nightly workflows, and regression tests from
  saved reproducers.
- [docs/example-kv.md](docs/example-kv.md): a key-value store and a power cut: two
  storage bugs found, shrunk and read.
- [API reference](https://felixmiddelhoff.github.io/ravel/) (also: `doxygen docs/Doxyfile`,
  or the `ravel_docs` CMake target, output in `build-docs/html`).
- [CHANGELOG.md](CHANGELOG.md): what changed in each release.
- [docs/formats.md](docs/formats.md): the trace file, the choices file, and how
  the trace digest is computed.
- [docs/disk-model.md](docs/disk-model.md): the exact rules of the virtual disk, how they
  compare with real filesystems, and how they are tested.
- [docs/abi-policy.md](docs/abi-policy.md): what may change in the C ABI.

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
