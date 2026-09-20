# Contributing to ravel

## Building and testing

```bash
cmake -S . -B build -DRAVEL_SHARED=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Format before committing:

```bash
clang-format -i include/ravel/*.hpp src/*.cpp tests/*.[ch]pp examples/*.cpp
```

`.clang-format` and `.clang-tidy` at the repo root pin the style/lint rules;
most editors pick them up automatically.

## How the code is organized

Everything public is in `include/ravel/`, with one `.cpp` per header in `src/`.
Read them in this order and the design falls out:

| Header | What it is |
|---|---|
| `rng.hpp` | `VirtualRng`: the one source of randomness. Every draw is a bounded integer (0 is the simplest outcome) that is recorded, and a recorded list can be replayed. The heart of the design. |
| `clock.hpp`, `trace.hpp` | Virtual time, and the record of a run (an event list plus its digest). |
| `task.hpp`, `scheduler.hpp` | Coroutine tasks and the scheduler that picks which ready task runs next (a choice), with a timer queue for virtual time. |
| `network.hpp`, `disk.hpp` | `Channel` and `Disk`: virtual I/O with fault injection. Both are built on `Scheduler::call_after` and draw every fault from the rng. |
| `simulation.hpp` | `Simulation`: owns one run's clock, rng, trace, scheduler, channels and disks, and checks invariants at the end. |
| `runner.hpp`, `shrink.hpp` | `run_seeds` (many seeds in parallel) and `shrink`/`replay` (minimize a failure by editing its choice list). |
| `determinism.hpp`, `sweep.hpp` | `check_determinism`, and the ready-made command line for test programs. |
| `ravel.h`, `src/c_api.cpp` | The small C ABI; see `docs/abi-policy.md`. |

**How a run flows.** `Simulation::run_until_quiescent` calls
`Scheduler::run_until_quiescent`, which loops: if nothing is ready, fire the
earliest timers (advancing the virtual clock); otherwise draw a choice, pick a
ready task, and resume it until its next `co_await`. Channel deliveries and disk
completions are timers. Each step is recorded in the `Trace`.

**The rules that keep it deterministic** (breaking one breaks replay):

- Every random decision goes through `VirtualRng`. Never use `std::rand`,
  `std::random_device`, `<random>` engines or distributions.
- No wall-clock time, no real threads, no real I/O inside a simulation.
- Iterate ordered containers (`std::map`, vectors), never ones ordered by hash or
  pointer, when the order can affect the run.
- A change to *which* draws happen, or in *what order*, changes what every seed
  does. That is allowed before 1.0 but must be listed in `CHANGELOG.md`, and the
  golden tests (`tests/test_rng.cpp`, `tests/test_simulation.cpp`) must be
  updated on purpose.

**Adding a fault or a feature.** Put its randomness behind `VirtualRng` with 0 as
the simplest outcome (so shrinking steers toward it), record any new kind of
event in `Trace` (and in `docs/formats.md`), add tests, and extend
`bench/soak.cpp` so the soak run exercises it.

## Conventions

- Public API comments are Doxygen comments (`///`, and `///<` after a member).
  The API reference is built from them, and CI fails the docs build if a public
  class or member has none.
- Tests use the small harness in `tests/testing.hpp` (`TEST(name) { CHECK(...); }`);
  checks stay active in Release builds.
- A new user-facing feature gets a mention in `CHANGELOG.md` under "Unreleased".

## Documentation

The tutorial, porting guide and debugging guide quote real code and real
program output from `docs/snippets`. After changing either, refresh the docs
and review the diff:

```bash
cmake --build build
python3 tools/check_docs.py --build-dir build --update
```

CI runs the same script without `--update` and fails if the docs are out of
date, so the documentation cannot drift from what the code does.

## The property that must never regress

`VirtualRng` and `VirtualClock` being seed-deterministic — same seed, same
output, on every platform — is the one guarantee the entire library exists
to provide. Any change touching `include/ravel/rng.hpp`, `clock.hpp`, or
`src/scheduler.cpp`'s interleaving logic needs a test proving determinism
still holds, not just that the change compiles.

## Submitting a pull request

1. Open an issue first for anything non-trivial (a new fault-injection
   type, a change to the scheduler's interleaving algorithm, a new C ABI
   function) — cheap to discuss before code exists.
2. Keep PRs scoped to one change.
3. CI must pass (build matrix + sanitizers) before merge.
4. New public API (`include/ravel/`) needs a test and a README update in
   the same PR: docs and code change together, not in a follow-up.

## Releasing

1. Move the "Unreleased" entries in `CHANGELOG.md` under a new version heading and
   date, and update the compare links at the bottom.
2. Bump the version in `CMakeLists.txt` (`project(... VERSION x.y.z)`),
   `include/ravel/version.hpp`, `src/version.cpp`, `packaging/conan/conanfile.py`
   and `packaging/vcpkg/ports/ravel/vcpkg.json`. (A test checks that
   `version.hpp` and `version.cpp` agree.)
3. Run the whole test suite, then start the `soak` workflow (Actions tab, or
   `gh workflow run soak.yml`) and wait for all three jobs: the soak on Linux, macOS and
   Windows, the thread-pool stress under ThreadSanitizer, and the sanitized soak. Release
   only if it is green, and link the run in the release notes. For a bigger local run:
   `ravel_soak 100000`.
4. Tag `vX.Y.Z` and push the tag, then create the release.
5. Compute the SHA-512 of the tag's tarball and put it in
   `packaging/vcpkg/ports/ravel/portfile.cmake`; verify with a local vcpkg install.

Never tag 1.0 without the maintainer's explicit go-ahead: from 1.0 the API, the C
ABI and the meaning of a seed are promises.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
