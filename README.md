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
ravel::Simulation sim(seed);

int counter = 0;
for (int i = 0; i < 3; ++i) {
  sim.scheduler().spawn("incrementer", [&]() -> ravel::Task {
    const int seen = counter;
    co_await sim.scheduler().yield();  // the scheduler may run any task here
    counter = seen + 1;                // lost update if one did
  });
}
sim.add_invariant("no_lost_updates", [&] { return counter == 3; });

ravel::Result r = sim.run_until_quiescent();
if (!r.ok) std::printf("seed %llu: %s
", r.seed, r.failure.c_str());
```

Run it over many seeds and some interleavings expose the bug; the failing
seed replays identically. See [`examples/quickstart.cpp`](examples/quickstart.cpp).

Tasks are C++20 coroutines. At every `co_await scheduler.yield()` or
`co_await scheduler.sleep(ticks)` the seeded scheduler picks which runnable
task goes next. Time is virtual: when every task sleeps, the clock jumps
to the earliest wake-up.

## Status of the seams

| Seam | Today | Planned |
|---|---|---|
| `VirtualClock` / `VirtualRng` | done, seed-deterministic | - |
| `Scheduler` | seed-driven interleaving, virtual-time sleep | - |
| `Trace` | in-memory event log with a replay digest | dump to a file on failure |
| `Channel` / `FaultSpec` | struct defined, not wired up | loss/latency/reorder actually applied |
| Multi-seed runner, shrinking | not started | planned |
| Disk/filesystem faults | not started | planned |

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
