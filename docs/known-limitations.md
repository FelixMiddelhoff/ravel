# Known limitations

ravel is well tested for a 0.x library. It is not proven correct, and it only
finds bugs that its models can express. This page lists what to keep in mind, in
the order you are likely to run into it.

## What "tested" means here

- The test suite, the property tests, the soak run and the disk reference-model
  comparison are all written by the author of the code. Shared blind spots are
  possible: a wrong assumption made in the code may have been made in its tests too.
- Line and branch coverage of the library are measured in CI, with a floor that
  fails the build. Mutation testing (Mull) is run by hand, not in CI; the surviving
  mutants were each judged equivalent or harmless, and the judgements are reasoned,
  not proven, apart from the thread-related ones, which were also run under
  ThreadSanitizer.
- The two input parsers (choices files and sweep command-line flags) were fuzzed
  for an hour each. That finds crashes, not wrong answers.
- The soak run checks that ravel agrees with itself (a seed reproduces, recorded
  choices replay). A behavior that is wrong but repeatable passes it.
- Nobody outside this repository has used or reviewed ravel yet.

## The models are simplifications

A bug that needs a fault ravel does not model will not be found, and a bug in a model
can make ravel report something a real system cannot do, or hide something it can.

**Disk.** [The disk model](disk-model.md) states every rule, where it is stricter or
looser than real filesystems, and what is not modeled (truncate, append, links,
permissions, bit rot, misdirected writes, read errors, and more). It has not been
checked against real hardware or a real filesystem; it is checked against a second
implementation of the same rules written by the same author.

**Network.** A channel is one-way and in memory. It can lose messages, delay them by a
uniform amount, and (if you allow it) reorder them. Not modeled: duplicated or corrupted
messages, bandwidth limits, connection setup and teardown, and partitions as a built-in
concept (you can model one by dropping messages from your own code). Each channel is
independent of the others.

**Time.** Virtual, and it moves only when every task is waiting. There is no clock skew
or drift between nodes, and a task that computes for a long time takes no virtual time.

**Scheduling.** Tasks are cooperative coroutines on one thread. A task is only
interrupted where it awaits, so a race that needs preemption inside a plain function is
not found. Real threads inside the code under test make runs unrepeatable.

**Process faults.** A crash is modeled for the disk (a power cut) and by however you
stop and restart your own tasks. Memory corruption, Byzantine behavior, and faults in
the operating system are not modeled.

## Determinism has to come from you

A seed reproduces a run only if nothing outside ravel influences the code under test:
wall-clock time, `rand()`, threads, globals that survive between runs, pointer-keyed
hash maps and similar. `check_determinism` catches many of these, and `shrink` refuses
to shrink a failure that does not reproduce, but neither can prove that nothing leaks.
The list is in [porting.md](porting.md#is-it-really-deterministic).

The same seed gives the same run on the platforms in CI (Linux, macOS, Windows, and a
32-bit x86 Linux build), checked by golden tests on the random stream and on trace
digests. No big-endian target is tested. The digest and the random generator use integers
and bytes only, so results should not depend on byte order, but that is an argument, not a
test.

## Shrinking

- A shrunk failure is smaller than the original, not always the smallest possible. The
  search is a heuristic with an attempt budget, and it reports when the budget ran out.
- Shrinking changes the recorded choices, so a shrunk run can fail for the same invariant
  through a different path than the original.
- Different thread counts give the same answer (tested), but a change to ravel's search
  can change which smaller failure you get.

## Stability

ravel is pre-1.0. Between minor versions the API, the C ABI and the meaning of a seed
may change, and a seed or choices file from one version may not reproduce on another;
`CHANGELOG.md` says when that happens. From 1.0 these become promises; see
[abi-policy.md](abi-policy.md).

## Not for these

ravel does not intercept system calls or run an existing binary unchanged, does not
touch a real network or disk, does not support multithreaded code under test, and its
random generator is not for keys, nonces or tokens. See [concepts.md](concepts.md).
