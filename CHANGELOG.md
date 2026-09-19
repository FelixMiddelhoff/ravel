# Changelog

All notable changes to ravel are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/) with one caveat: **before 1.0, a minor
version may break the API, the meaning of a seed, and the C ABI.** Each such break
is listed under "Changed" or "Removed" in the release that makes it.

## [Unreleased]

### Added

- CMake option `RAVEL_COVERAGE` and a CI job that builds with coverage, runs the
  tests and uploads an HTML report.

### Fixed

- `tools/ravel_trace.py` crashed with a Python traceback on some damaged trace files
  (invalid UTF-8, a header or event that is not a JSON object, an event with a
  missing or wrongly typed field, absurdly deep nesting). It now reports each with
  exit status 2. Found by `tools/fuzz_trace.py`, which is also a unit test.
- `shrink` (and so `run_seeds` with `shrink_first_failure`, and `--replay`) could not
  minimize a run that threw after making random choices: the choices were lost and
  shrinking failed with "the setup is not deterministic". The choices made before the
  throw are now kept. Found by the new thread-pool stress tests.

## [0.3.0] - 2026-09-19

### Added

- `ravel::run_sweep_main` and `ravel::run_sweep`: a ready-made command line for
  simulation test programs (`--seeds`, `--first-seed`, `--threads`, `--trace-dir`,
  `--no-shrink`, `--time-limit`, `--replay FILE`, `--check-determinism`), with exit
  statuses for CI and error annotations on GitHub Actions.
- `ravel::check_determinism`: runs each seed twice and once more replayed from its
  choices, and names the first step where the runs differ.
- `Simulation::describe(event)` and `TraceEvent::operator==`.
- `tools/ravel_trace.py`: `summary`, `show`, `timeline` and `diff` for trace files.
- Documentation: a tutorial, a porting guide, a debugging guide, a concepts page,
  a CI guide, a key-value store walkthrough, a comparison with related tools, and a
  troubleshooting guide. Every code and output block is generated from real programs,
  and every error message quoted is looked up in the source, both checked in CI
  (`tools/check_docs.py`).
- The API reference is published to GitHub Pages.

### Changed

- Public headers use Doxygen doc comments (`///`), so the API reference shows the
  descriptions. No behavior change: **seeds and choice lists from 0.2.0 still mean
  the same runs.**

## [0.2.0] - 2026-09-19

### Added

- Directories and rename for the virtual disk: `rename`, `remove`, `sync_dir` and
  `list`. Directory changes are durable only when synced, and a crash keeps some
  in-order prefix of the pending ones. `Disk::crash_count()` lets a task notice that
  a crash killed it.
- `Channel::receive_within` (receive with a timeout), `Channel::clear_inbox`, and
  `SimulationOptions::time_limit` for systems that never go quiet.
- Shrinking replays candidates in parallel (the result does not depend on the
  thread count) and has a new pass that lowers two choices together or moves value
  between them. `ShrinkOptions::threads`.
- A working Raft example (leader election, log replication, state persisted through
  the virtual disk, crashing nodes) with three deliberate bugs that ravel finds.
- Install rules and a CMake package (`find_package(ravel)`); a Conan recipe and a
  vcpkg overlay port.
- A soak test (`ravel_soak`) that checks ravel's own replay promises over many seeds.
- A single-thread guard: using a running simulation from another thread throws.

### Changed

- Reading a missing file returns `NotFound` instead of an empty result.
- A disk crash now also decides which pending directory changes survive. **Seeds
  and choice lists from 0.1.0 no longer mean the same runs.**
- The Raft example replaces the quorum-register example.

### Fixed

- `shrink()` checked that a failure was reproducible using default options instead
  of the run's own (for example, without its time limit).
- `ravel_simulation_create` could let an exception escape the C boundary.

## [0.1.0] - 2026-09-19

### Added

- The first release: a C++20 coroutine scheduler with seeded interleaving and
  virtual time, virtual channels with loss, latency and reordering, virtual disks
  with crashes and torn writes, a parallel multi-seed runner, choice-stream
  shrinking with replayable reproducers, JSON Lines traces, and a small C ABI.

[Unreleased]: https://github.com/FelixMiddelhoff/ravel/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/FelixMiddelhoff/ravel/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/FelixMiddelhoff/ravel/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/FelixMiddelhoff/ravel/releases/tag/v0.1.0
