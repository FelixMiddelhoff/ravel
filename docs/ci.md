# Running ravel in CI

A simulation test is an ordinary program that exits non-zero when a seed fails,
so it fits any CI system. This page gives you the pieces: a test program with a
ready-made command line, workflows for pull requests and nightly runs, a place
for reproducers to live, and advice on how many seeds to run.

- [The test program](#the-test-program)
- [Pull requests: a quick sweep](#pull-requests-a-quick-sweep)
- [Nightly: a deep sweep](#nightly-a-deep-sweep)
- [Regressions: reproducers that never go away](#regressions-reproducers-that-never-go-away)
- [With your test framework](#with-your-test-framework)
- [How many seeds?](#how-many-seeds)
- [When a run fails in CI](#when-a-run-fails-in-ci)

## The test program

`ravel::run_sweep_main` gives your program a complete command line. This is the
whole `main`:

<!-- snippet: docs/snippets/ci_sweep.cpp#ci_main -->
```cpp
int main(int argc, char** argv) {
  // The tutorial's deposit system, still with its bug.
  return ravel::run_sweep_main(argc, argv, deposit_setup(/*dedupe=*/false));
}
```

(`deposit_setup` is the system from the [tutorial](tutorial.md#3-the-system-to-test),
still with its bug, so we have something to find.) Run it with no arguments and you
get a sweep of 1000 seeds. Here is the same program with 200 seeds:

<!-- output: ci_sweep --seeds 200 -->
```text
FAILED: 50 of 200 seeds
first failure: seed 6: invariant 'deposit_applied_at_most_once' failed
shrunk from 11 random choices (9 steps) to 4 (6 steps)
reproducer: ravel-traces/ravel-seed-6.choices
replay it:  ci_sweep --replay ravel-traces/ravel-seed-6.choices
trace:      ravel-traces/ravel-seed-6.replay.trace.jsonl
```

The summary tells you what failed, gives you the reproducer and trace, and says
exactly how to **replay** it. Exit status is `1`. When everything passes it is `0`,
and `2` means a bad command line.

The options you will use in CI:

<!-- output: ci_sweep --help -->
```text
usage: ci_sweep [options]

Runs the simulation under many seeds and reports any that fail.

  --seeds N                how many seeds to run (default 1000, or $RAVEL_SEEDS)
  --first-seed S           the first seed (default 0, or $RAVEL_FIRST_SEED)
  --threads N              worker threads (default: one per hardware thread)
  --trace-dir DIR          where failures are saved (default ravel-traces)
  --no-shrink              do not minimize the first failure
  --max-shrink-attempts N  budget for shrinking
  --time-limit TICKS       stop each run at this virtual time
  --replay FILE            replay a saved .choices file instead of sweeping
  --check-determinism      check the code is deterministic instead of sweeping
  --help                   this text

Exit status: 0 all passed, 1 a failure was found, 2 bad command line.
```

Two of them come from the environment too, which is convenient in CI:
`RAVEL_SEEDS` and `RAVEL_FIRST_SEED` (a flag beats the variable).

## Pull requests: a quick sweep

On every pull request, run a modest number of seeds: enough to catch the common
bugs, fast enough that nobody waits. With GitHub Actions:

```yaml
name: simulation
on: [pull_request, push]

jobs:
  sweep:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build --target my_sim_test --parallel
      - name: Sweep 2000 seeds
        run: ./build/my_sim_test --seeds 2000
      - name: Keep the evidence
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: ravel-traces
          path: ravel-traces/
```

What you get when a seed fails:

- the job fails, and GitHub shows the failure as an **error annotation** on the run
  (ravel prints it in GitHub's format when it detects Actions), with the seed, the
  invariant that broke and where the reproducer is;
- the trace and the `.choices` reproducer are attached as a downloadable artifact.

Use a `Release` build for the sweeps: they are compute-bound and run many times
faster than `Debug`. Keep a separate, smaller job in `Debug` with sanitizers
(ASan, UBSan) if you use them elsewhere, since a memory error inside a simulated
task is a real bug.

## Nightly: a deep sweep

New seeds are new chances to find something, and nobody is waiting overnight. Run a
much bigger sweep on a schedule, and start at a **different seed each night** so
you never repeat yourself:

```yaml
name: nightly-simulation
on:
  schedule:
    - cron: "17 2 * * *"      # every night, 02:17 UTC
  workflow_dispatch:           # and on demand

jobs:
  deep-sweep:
    runs-on: ubuntu-latest
    timeout-minutes: 120
    steps:
      - uses: actions/checkout@v4
      - name: Build
        run: |
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
          cmake --build build --target my_sim_test --parallel
      - name: Sweep 500000 seeds, a fresh slice each night
        run: |
          ./build/my_sim_test --seeds 500000 \
            --first-seed $(( ${{ github.run_number }} * 1000000 ))
      - name: Keep the evidence
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: ravel-traces
          path: ravel-traces/
```

`github.run_number` goes up by one each run, so run 41 covers seeds 41,000,000 to
41,499,999. Nothing overlaps, and any failure names its seed, so you can always
come back to it.

## Regressions: reproducers that never go away

When the nightly sweep finds something, you fix it *and* keep its reproducer, so
the bug can never quietly return. Keep the shrunk `.choices` file in your
repository (say under `tests/regressions/`) and replay it as a test. The
`--replay` flag does exactly that, and exits non-zero if the run fails:

<!-- output@root: ci_sweep --replay docs/snippets/data/deposit_minimal.choices -->
```text
replay of docs/snippets/data/deposit_minimal.choices (4 choices): FAILED: invariant 'deposit_applied_at_most_once' failed
```

(This one *fails*, because the tutorial's server still has its bug. After the fix it
passes, and stays a test.) With CMake, register every file in the folder as a test
in one go:

```cmake
enable_testing()
add_test(NAME simulation_sweep COMMAND my_sim_test --seeds 1000)

file(GLOB REGRESSIONS ${CMAKE_CURRENT_SOURCE_DIR}/regressions/*.choices)
foreach(file ${REGRESSIONS})
  get_filename_component(name ${file} NAME_WE)
  add_test(NAME regression_${name} COMMAND my_sim_test --replay ${file})
endforeach()
```

Now `ctest` runs the sweep and every regression, locally and in CI. A new bug is a
new file dropped into the folder. (Re-run CMake to pick it up.)

A reproducer replays **the structure of the run**, so it keeps working when ravel
is upgraded, but it can stop *failing* if you change the code under test so much
that the recorded choices no longer line up with what the code asks. That is not a
regression to worry about: if the fix or a refactor makes a reproducer pass, it
still guards the case it was written for as well as it can, and the sweep guards the
rest.

## With your test framework

`run_sweep` is the easy way in, but you can also call the pieces directly from
gtest, Catch2, doctest or anything else. The two calls are `ravel::run_seeds` (a sweep)
and `ravel::replay` (a saved reproducer):

```cpp
// gtest
TEST(Deposit, SurvivesTheNetwork) {
  ravel::RunnerOptions options;
  options.seed_count = 1000;
  options.simulation.trace_dir = "ravel-traces";
  const ravel::RunnerReport report = ravel::run_seeds(deposit_setup(true), options);
  ASSERT_TRUE(report.ok()) << report.failures.front().failure
                           << " (seed " << report.failures.front().seed << ")";
}

TEST(Deposit, RegressionDoubleDeposit) {
  std::ifstream file("regressions/double_deposit.choices");
  const ravel::Result result = ravel::replay(deposit_setup(true), ravel::read_choices(file));
  EXPECT_TRUE(result.ok) << result.failure;
}
```

```cpp
// Catch2
TEST_CASE("deposit survives the network") {
  ravel::RunnerOptions options;
  options.seed_count = 1000;
  const auto report = ravel::run_seeds(deposit_setup(true), options);
  INFO((report.ok() ? "" : report.failures.front().failure));
  REQUIRE(report.ok());
}
```

The tutorial's [regression example](tutorial.md#8-keep-it-fixed) is the same idea
in plain code. (The framework snippets above are not compiled by this repository's
docs build, which has no dependency on gtest or Catch2; they use only calls that
appear in the compiled examples.)

## How many seeds?

There is no magic number, only a trade of time against chances. Think in **time
budgets**, not seed counts:

| Where | Budget | Why |
|---|---|---|
| Local, while developing | a few seconds | fast feedback: catches the common bugs |
| Every pull request | 1 to 5 minutes | most bugs need one unlucky moment and show up on a few percent of seeds |
| Nightly | 30 to 120 minutes | rare bugs: the rarest one in ravel's own Raft example fails about 1 seed in 600 |

To turn a budget into a number, measure once: run `time ./my_sim_test --seeds 1000` and
divide. Simple systems run many thousands of seeds a second; a much heavier one such
as the Raft example (three nodes, crashes, disks) manages on the order of a hundred
a second across all the cores of a laptop. Then pick the count that fills
your budget. `--threads` defaults to every core, so a bigger CI machine helps almost
linearly.

If a bug only shows up on rare seeds, more seeds is the answer. If it never shows up
at all, ask whether your **faults** are reaching it: a system that never sees a lost
message or a crash cannot fail because of one. Raise loss rates, add crashes, add
latency variance. ravel's own examples use fairly harsh settings for that reason.

Two limits worth setting in the setup you give `run_sweep_main` (through
`SweepDefaults::simulation`, or on the command line):

- **`time_limit`**, for systems that never go quiet (heartbeats, election timers): the
  run stops at that virtual time and the invariants are checked. `--time-limit`
  overrides it per invocation.
- **`max_steps`** (a million by default) turns a livelock into a failure instead of a
  hung CI job. Raise it only if your system is legitimately that busy.

## When a run fails in CI

1. Download the `ravel-traces` artifact. It has the trace (`*.replay.trace.jsonl`, the
   minimal run) and the reproducer (`*.choices`).
2. Reproduce it on your machine, with the exact command from the output:
   `./my_sim_test --replay ravel-traces/ravel-seed-N.choices`. Same choices, same
   run, every time.
3. Read what happened with the [trace tool](debugging.md#reading-traces-with-ravel_trace)
   and fix the bug.
4. Copy the `.choices` file into `regressions/`. It is now a test.

If the failure does not reproduce locally, the code under test is not fully
deterministic. Run `./my_sim_test --check-determinism`: it names the first step where
two runs of the same seed part ways. See the [porting guide](porting.md#is-it-really-deterministic).
