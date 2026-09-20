# Concepts: how ravel works

A one-page mental model. Read it once and the rest of the documentation will make
more sense, because everything else is a consequence of the ideas here.

- [The picture](#the-picture)
- [What happens in a run](#what-happens-in-a-run)
- [One source of randomness](#one-source-of-randomness)
- [Seed, choices, trace, digest](#seed-choices-trace-digest)
- [Why shrinking works](#why-shrinking-works)
- [What ravel is not](#what-ravel-is-not)
- [Glossary](#glossary)

## The picture

```
 ┌────────────────────────── Simulation (one run, one seed) ──────────────────────────┐
 │                                                                                    │
 │   your tasks ───────►  Scheduler  ◄──── VirtualClock     (time only moves when     │
 │   (C++20 coroutines)   picks who        virtual ticks     every task is waiting)   │
 │        │  ▲            runs next                                                   │
 │        │  │                 │                                                      │
 │  send ─┘  └─ receive        │ every "who next?", "is it lost?", "how long?"        │
 │        ▼                    ▼                                                      │
 │   Channel(s)  Disk(s)    VirtualRng  ── one seeded source, every draw recorded ──► │  choices
 │   (network faults) (crash, torn writes)                                            │
 │                                                                                    │
 │   invariants ── checked when the run ends ──►  Result { ok, failure, trace ... }   │
 └────────────────────────────────────────────────────────────────────────────────────┘
```

Your code runs *inside* the box, and everything it can observe (the time, other
tasks, the network, the disk, random numbers) is produced by ravel. Nothing from
outside gets in, so **a run is a pure function of its seed**.

## What happens in a run

1. Your **setup** function builds the system: it creates channels and disks,
   spawns tasks and registers invariants.
2. The **scheduler** repeats one step over and over: from the tasks that are ready
   to run, pick **one** (that pick is a random *choice*), and run it until it
   reaches a `co_await`.
3. A `co_await` is where a task hands control back: `sleep`, `yield`,
   `channel.receive()`, a disk operation. The task is parked until whatever it
   waits for happens.
4. When **no task is ready**, the clock jumps straight to the next timer (a sleeping
   task waking, a message arriving, a disk operation finishing). Simulated waiting
   costs no real time.
5. When nothing is ready *and* no timer is pending, the run is over. (For systems that
   never go quiet, such as heartbeats, `time_limit` ends it.)
6. **Invariants** are checked, and you get a `Result`.

The interesting bugs live between steps 2 and 3. A `co_await` is a point where
*another task may run*, and the scheduler tries different orders on different seeds.
Two operations with no `co_await` between them cannot be interleaved; put one in
(`co_await sim.scheduler().yield()`) if you want ravel to try reordering around it.

## One source of randomness

Three things need randomness, and all of them draw from the same recorded source:

- the **scheduler**, when it picks which ready task runs next;
- the **faults**, in channels (is this message lost? how late is it?) and disks
  (does this write survive a crash? is it torn?);
- **your code**, through `sim.rng()`, for workloads and jitter.

Every draw is a bounded whole number, and **0 is always the simplest outcome**:
the task that has waited longest, no fault, the shortest delay. The list of
everything a run drew is its list of *choices*. Here is one, printed, replayed and
changed:

<!-- snippet: docs/snippets/concepts_choices.cpp#concepts_main -->
```cpp
int main() {
  const ravel::SimulationSetup setup = deposit_setup(/*dedupe=*/false);

  // 1. Run seed 6 and ask what it decided.
  ravel::Simulation sim(6);
  setup(sim);
  const ravel::Result original = sim.run_until_quiescent();
  print_choices("seed 6 made these choices", sim.choices());
  std::printf("  -> ok=%d, %llu steps, digest %016llx\n", original.ok,
              static_cast<unsigned long long>(original.steps),
              static_cast<unsigned long long>(original.trace_digest));

  // 2. Replay exactly that list. No seed involved: the same run comes back.
  const ravel::Result again = ravel::replay(setup, sim.choices());
  std::printf("replaying the list: digest %016llx (%s)\n",
              static_cast<unsigned long long>(again.trace_digest),
              again.trace_digest == original.trace_digest ? "identical" : "different");

  // 3. Change the list, and you have changed the run: all zeros is the
  //    simplest run there is (first task, no faults, shortest delays).
  const ravel::Result simplest = ravel::replay(setup, ravel::Choices{});
  std::printf("an empty list (all zeros): ok=%d, %llu steps, digest %016llx\n", simplest.ok,
              static_cast<unsigned long long>(simplest.steps),
              static_cast<unsigned long long>(simplest.trace_digest));
  return 0;
}
```

<!-- output: concepts_choices -->
```text
seed 6 made these choices: 1 0 0 1 1 1 1 0 0 0 6
  -> ok=0, 9 steps, digest 4efcf9a723b62dfd
replaying the list: digest 4efcf9a723b62dfd (identical)
an empty list (all zeros): ok=1, 4 steps, digest 17eefd4bdd1c1564
```

The list `1 0 0 1 1 1 1 0 0 0 6` **is** the run: replaying it gives the identical run
(same digest) without any seed. An empty list means every draw takes its default, 0,
which is the plainest run there is, here one that does not hit the bug. Anything in
between is another run. That is what makes shrinking possible.

## Seed, choices, trace, digest

Four words, four jobs:

| | What it is | What you use it for |
|---|---|---|
| **seed** | a number that determines the whole run | naming a run: "seed 6 fails". Convenient, but its meaning can change when ravel is upgraded |
| **choices** | the list of every draw the run made | reproducing a run exactly; the shrunk list is your **saved reproducer** and stays valid across ravel upgrades |
| **trace** | one line per event: who ran when, which message was lost, which disk write completed | *reading* what happened; see [debugging.md](debugging.md) |
| **digest** | a 64-bit fingerprint of the trace | asking "are these two runs identical?" without comparing them event by event |

## Why shrinking works

A failing run starts as a long list of choices, most of which have nothing to do
with the bug. ravel looks for a shorter, simpler list that still fails **the same
way**:

- delete some numbers (fewer decisions);
- set some to 0 (no fault here, first task there);
- lower others (shorter delays);
- lower two together, or move value from one to another, for failures that need two
  numbers to add up.

Each candidate list is *replayed*. If it still fails with the same message, it
becomes the new best; if not, it is discarded. It stops when nothing simpler
works. Because every list is a valid run (numbers too large are clamped, missing
ones are 0), ravel can edit freely, and because runs are deterministic, the
answer never depends on luck or on how many threads did the replaying.

The nonzero numbers left over are the **ingredients of the bug**. In the tutorial,
`0 0 0 1` reads "deliver the request, lose the reply": that is the whole bug.

## What ravel is not

- It does not intercept system calls or run your existing binary unchanged. You give
  your code a seam (time, randomness, network, disk) to plug ravel into; see the
  [porting guide](porting.md).
- It models the faults you tell it about: message loss, delay, reordering, crashes,
  torn writes, full disks, I/O errors. A bug that needs a fault ravel does not model
  will not be found.
- It is single-threaded per run. Real threads inside the code under test make runs
  unrepeatable.
- It has known limits, listed in [known-limitations.md](known-limitations.md).
- It is not a proof. Passing a million seeds means no bug was found in a million
  tries, which is a lot more than a few hand-written tests, but it is not "none exist".

## Glossary

**Channel**: a one-way virtual network link between two named endpoints, with a
`FaultSpec` (loss, delay, reordering).
**Choice**: one recorded random draw. A run is its list of choices.
**Determinism**: same seed, same run, on every machine. The foundation of everything.
**Digest**: a fingerprint of a run's trace; equal digests mean identical runs.
**Disk**: a virtual filesystem with a write-back cache, `sync`, `rename`, `sync_dir`,
and crashes that lose or tear what was not synced.
**Invariant**: a property that must hold when the run ends.
**Replay**: run a saved list of choices instead of a seed.
**Reproducer**: the shrunk choices of a failure, saved as a `.choices` file.
**Run**: one execution of your system under one seed (or one list of choices).
**Scheduler**: what picks which ready task runs next.
**Seed**: a number that determines a whole run.
**Setup**: your function that builds the system for one run.
**Shrinking**: finding the simplest run that still fails the same way.
**Simulation**: the object that owns a run's clock, randomness, scheduler, channels
and disks.
**Sweep**: running many seeds of the same setup.
**Task**: a C++20 coroutine that is part of your system.
**Trace**: the record of a run, one event per line.
**Virtual time**: simulated time in ticks; it only moves when every task is waiting.
