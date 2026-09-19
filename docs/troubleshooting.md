# Troubleshooting

Something went wrong: an error message, a crash, a result that surprises you. Find
it here. Entries are grouped by where you meet the problem.

- [Building](#building)
- [Messages from ravel](#messages-from-ravel)
- [Command-line errors](#command-line-errors)
- [Symptoms without a message](#symptoms-without-a-message)

Each message below is checked against the source by `tools/check_docs.py`, so what
you read here is what ravel really prints. For what the *failure* messages of a
run mean (an invariant failed, a livelock, and so on) see the
[debugging guide](debugging.md#what-each-failure-looks-like).

## Building

**Compile errors about `co_await`, `co_return` or `<coroutine>`.** ravel needs C++20
with coroutines: GCC 10 or later, Clang 14 or later, or MSVC 2019 or later, built as
C++20. With CMake, `target_compile_features(my_test PRIVATE cxx_std_20)` does
it. GCC 10 also needs `-fcoroutines`; GCC 11 and later turn coroutines on with
`-std=c++20`.

**Linker errors about `pthread_create` or `std::thread` on Linux.** Link
`ravel::ravel` through CMake (it brings the thread library along). If you link by
hand, add `-pthread`.

**An error about designated initializers (`{.loss_probability = 0.3}`).** These are
C++20. Make sure the whole target is built as C++20, not only the ravel library.

**The tutorial's `FetchContent` builds tests and examples too.** It should not: they
are off when ravel is a subproject. If you see them building, check that you use
`FetchContent_MakeAvailable` and not a manual `add_subdirectory` with options set.

## Messages from ravel

### The setup is not deterministic

<!-- message: did not reproduce its failure; the setup is not deterministic -->
```text
ravel::shrink: replaying seed 7 did not reproduce its failure; the setup is not deterministic
```

`shrink` replays a failure to minimize it, and this one did not happen the second
time. Something outside ravel influences the code under test. Run
`ravel::check_determinism` (or `--check-determinism` in a `run_sweep` program): it names
the first step where two runs of the same seed part ways. See
[the checklist](porting.md#is-it-really-deterministic).

### A determinism check found a difference

<!-- message: diverged at step -->
<!-- message: one run ended early -->
<!-- message: the runs made the same moves but -->
<!-- message: something other than ravel's random choices is steering the run -->

`check_determinism` reports one of these, each naming a step and the two events that
differ:

- **`two runs of the same seed diverged at step N`**: the code is not repeatable. The
  step names the first event that differs. Look at what feeds that task's timing or
  decisions: a clock, a static, a container ordered by pointer.
- **`one run ended early`**: one run had more events than the other, so something
  changed the number of steps, often state left over from the previous run.
- **`the runs made the same moves but ... ended with ...`**: the events are identical
  but an invariant gave a different answer, so the invariant reads something that
  differs between runs (a static counter, a real clock).
- **`replaying run 1's recorded choices ... (something other than ravel's random
  choices is steering the run)`**: the run depends on something ravel does not
  record, so a saved `.choices` file would not reproduce it.

<!-- message: an invariant reads something that differs -->

### The simulation threw

<!-- message: simulation threw: -->
```text
simulation threw: bad setup
```

Your setup function (or the run itself) threw an exception. In a sweep that seed
fails, and the rest carry on. The text after the colon is the exception's `what()`.
Setup runs on several threads at once in `run_seeds` and `shrink`, so an exception
that appears only sometimes usually means the setup touches shared state.

<!-- message: simulation threw an unknown exception -->

`simulation threw an unknown exception` is the same, for an exception that is not a
`std::exception`.

### A task threw

<!-- message: ' threw: -->
```text
task 'account_service' threw: no such account: 42
```

A task let an exception escape, which stops the run at that step. If the exception is
meant to be part of the test, catch it inside the task.

### The step limit

<!-- message: possible livelock -->
```text
step limit of 1000000 reached (tasks are still runnable; possible livelock)
```

Tasks kept running without the run ending: usually two tasks yielding to each other,
or a retry loop with no backoff. See [debugging.md](debugging.md#the-step-limit-was-reached).
Raise `SimulationOptions::max_steps` only if the system is legitimately that busy. If
it *never* goes quiet by design (heartbeats), set `time_limit` instead.

### A second task tried to receive

<!-- message: a second task is already waiting to receive -->
```text
Channel: a second task is already waiting to receive
```

A channel has at most one receiving task at a time. Give each receiver its own
channel (a common pattern is one inbox channel per node that everyone else sends to),
or funnel messages to one task that distributes them.

### The inbox was cleared

<!-- message: the inbox was cleared while a receive was about to return -->
```text
Channel: the inbox was cleared while a receive was about to return
```

`Channel::clear_inbox()` was called by a task other than the receiver, at the moment
the receiver had a message queued to take. Call `clear_inbox` from the receiving task
itself, for instance when it reboots after a simulated crash.

### Used from another thread

<!-- message: simulations are single-threaded -->
```text
Scheduler::spawn called from a thread other than the one running the simulation; simulations are single-threaded
```

Code under test started a real thread (or a thread pool) and used the simulation from
it. A simulation is cooperative and single-threaded; replace the thread with a
ravel task (`sim.scheduler().spawn(...)`), and any real wait with `sleep` or
`receive_within`. (`call_after` is named instead of `spawn` when a channel or disk was
the thing used.)

### An invalid fault specification

<!-- message: loss_probability must be in [0, 1] -->
<!-- message: latency_min must not exceed latency_max -->
<!-- message: error probabilities must be in [0, 1] -->

`FaultSpec: loss_probability must be in [0, 1]`,
`FaultSpec: latency_min must not exceed latency_max`, and
`DiskFaultSpec: error probabilities must be in [0, 1]` (plus its own
`latency_min` message) mean what they say. They are thrown by `add_channel` and
`add_disk`.

### The trace could not be written

<!-- message: trace could not be written to -->
```text
invariant 'x' failed (trace could not be written to ravel-traces)
```

`trace_dir` is set but ravel could not create the directory or the file: check the
path is writable, and that nothing already exists there as a file. The failure itself
is unaffected; only the trace is missing.

### A choices file was rejected

<!-- message: not a ravel choice list (expected header 'ravel-choices 1') -->
<!-- message: ravel choice list is truncated or malformed -->

`read_choices` (and `--replay`) accept only what ravel wrote: the header
`ravel-choices 1`, a count, then that many whole numbers. A different version, a
missing header, a file cut short, or a value that is not a whole number is refused
with one of these two messages. See [formats.md](formats.md#choice-list-ravel-seed-seedchoices).

## Command-line errors

For programs built on `ravel::run_sweep_main`. Every one ends with
`Try 'my_test --help'.` and exit status 2.

<!-- message: unknown option -->
<!-- message: needs a value -->
<!-- message: needs a non-negative whole number -->
<!-- message: cannot open ' -->

- **`unknown option '--seed'`**: no such flag. The flag is `--seeds` (how many) or
  `--first-seed` (where to start). `--help` lists them all.
- **`--seeds needs a value`**: the flag was last on the line, or its value is missing.
- **`--seeds needs a non-negative whole number, got 'many'`**: numbers only.
- **`cannot open 'x.choices'`**: `--replay` could not open the file. Run from the
  directory the path is relative to, or pass an absolute path.

## Symptoms without a message

### A sweep passes, but a bug is surely there

Nothing forces a bug to show up: a run only finds what its *faults* and *invariants*
can reach.

- Does any run see the fault that triggers the bug? Raise loss rates, add latency
  variance, add crashes. A system that never sees a lost message cannot fail because
  of one.
- Is there an invariant that *would* notice? "Nothing bad happened" is not enough: add
  "the reply arrived" or "the write was acknowledged". See
  [the stuck-run case](debugging.md#everything-finished-but-nothing-happened).
- Are there `co_await`s between the steps that should be reorderable? Two operations
  with none between them cannot be interleaved.
- Are there enough seeds? A bug that needs one unlucky moment in six hundred needs
  more than a hundred seeds.

### It passes on my machine and fails in CI (or the reverse)

The same seed must give the same run everywhere. If it does not, something leaks into
the run. `--check-determinism` finds it. The usual suspects are uninitialized memory,
a container ordered by pointer, `static` state, and a real clock or random source.

### The program crashes, or reads garbage, inside a task

Almost always a **dangling reference**: a coroutine holding a reference to a local
variable of the setup function, which has returned by the time the task runs. Keep
state in `sim.make_state<T>()` and capture the reference it returns. See
[the pitfall](porting.md#pitfalls-with-coroutines). A debug build with AddressSanitizer
reports it precisely.

### The program hangs

Not the step limit (that fails cleanly), but a real hang: a task did something that
really blocks (a real `sleep_for`, a blocking socket read, waiting on a real mutex or
future). ravel is cooperative, so one blocked task stalls everything. Replace it with
`co_await sim.scheduler().sleep(...)` or a channel receive. A debugger's stack of the
hung program shows where.

### Shrinking finishes but the result still looks big

- Check `budget_exhausted` on the `ShrinkResult`: if it is true, shrinking stopped at
  `max_attempts` (20000 by default) and could go further with more.
- Otherwise the result is minimal *for that failure message*. Shrinking only accepts
  a smaller run if it fails with the same message, so it will not wander into a
  different bug. If the invariant that fails covers several causes, split it into
  invariants with distinct names.
- Remember that "steps" can go up while shrinking; the measure is the choice list.

### Shrinking is slow

Each attempt is a whole run, so long or heavy runs make shrinking slow. Shorten the run
(`time_limit`, fewer operations), lower `max_shrink_attempts`, or give it more threads:
`ShrinkOptions::threads` defaults to every core.

### The trace is enormous

It records every step. Shrink first (the minimal run is usually short), bound the run
with `max_steps` or `time_limit`, and use [ravel_trace.py](debugging.md#reading-traces-with-ravel_trace)
with `--name`, `--kind`, `--from` and `--to` to look at a slice.

### A saved reproducer stopped failing after a change

The choice list records *which random draws happen and in what order*. If you change
the code under test so that different draws happen (a new message, a different number
of tasks), the old list no longer describes the same run, and it may now pass. Find
the bug again with a sweep and save a fresh reproducer; keep the old one anyway, as it
still guards the exact run it captured as long as the structure is unchanged.

### The docs check fails in CI

`tools/check_docs.py` found a code or output block that differs from what the programs
now do. That is intended: run `python3 tools/check_docs.py --build-dir build --update`,
read the diff, and commit it if the new output is right.
