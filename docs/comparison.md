# ravel and the tools around it

ravel belongs to a family of ideas: run your system under a controlled, repeatable
version of the world, and test it against faults that real life delivers rarely and
at the worst moment. Other tools share pieces of that idea. This page says how ravel
relates to them and when another tool is the better choice.

These are short characterizations of each project's public documentation, meant to
place ravel, not to rank anything. Projects change; check the one you are
considering for what it does today.

- [The short version](#the-short-version)
- [Side by side](#side-by-side)
- [Notes on each](#notes-on-each)
- [When ravel is the wrong tool](#when-ravel-is-the-wrong-tool)
- [Using ravel with the others](#using-ravel-with-the-others)

## The short version

ravel is a **library for C++20 code** that puts your system in a deterministic
simulation (seeded scheduling, virtual time, a virtual network, virtual disks with
crashes) and, when a run fails, **shrinks** it to a minimal reproducer you can
replay. It asks you to give your code a seam to the outside world, and in return
needs no special runtime, no hypervisor, and no privileges, and works the same on
Linux, macOS and Windows.

## Side by side

| Tool | What it is | Target | Your code must change? | Same run again? |
|---|---|---|---|---|
| **ravel** | simulation library | C++20 | yes: time, randomness, network and disk go through ravel | yes, by seed or by saved choice list |
| **FoundationDB's simulator** | simulation built into one database | FoundationDB's own code (C++, Flow) | designed in from the start | yes, by seed |
| **TigerBeetle's VOPR** | simulation built into one database | TigerBeetle (Zig) | designed in from the start | yes, by seed |
| **turmoil** | network and time simulation for Tokio | Rust | yes: use its types for hosts and sockets | yes |
| **madsim** | deterministic replacement for async-runtime pieces | Rust | small: swap in its crates | yes |
| **Coyote** | controlled-scheduling tester for async code | .NET | small: run the test under its scheduler | yes |
| **loom** | exhaustive model checker for concurrent code | Rust | yes: use its atomics and threads in a test | yes, by exploring all interleavings |
| **Antithesis** | deterministic hypervisor, hosted | any software that runs in containers | no | yes |
| **rr** | record-and-replay debugger | native Linux programs | no | replays one recorded run |
| **Jepsen** | black-box testing of real clusters | any networked system | no | no: real clocks and networks |
| **Property-based testing** (QuickCheck, Hypothesis, RapidCheck) | generate inputs, shrink failures | functions and APIs, some stateful models | write generators | yes, by seed |

## Notes on each

**FoundationDB and TigerBeetle.** These are the origin of the approach that ravel
follows: their databases are written to run entirely inside a deterministic simulator,
which finds the bugs that hand-written tests miss. That works because *the whole
system was designed for it*. ravel is the same idea as a reusable library, for
C++ code you are adopting it into rather than writing for it from day one. If you are
starting a new storage or consensus system, building the simulation in from the start,
as they did, is the strongest position to be in; ravel gives you the machinery.

**turmoil and madsim (Rust).** The closest relatives: deterministic simulation for
async Rust, mostly by replacing the runtime, network and time. They are what a Rust
team reaches for; ravel is the C++ counterpart. What ravel puts weight on is storage (a disk model
with write-back caches, torn writes, crashes and directory operations) and
shrinking: a failure comes back as a minimal, saved, replayable list of choices.

**Coyote (.NET).** Takes async code and runs it under a scheduler that explores
interleavings and injects failures, with a strong focus on finding concurrency bugs
in cloud services. Similar in spirit to ravel's scheduler; a different ecosystem.

**loom (Rust).** A different tool for a different problem: it takes a *small
concurrent data structure* and exhaustively checks every interleaving of its threads
and atomic operations. It answers "is this lock-free queue correct?" ravel answers
"does this *distributed system* survive lost messages and power cuts?" ravel's tasks
are cooperative, so it does not test memory-ordering bugs between real threads; use a
model checker or ThreadSanitizer for those.

**Antithesis.** A hosted platform that runs *unmodified* software in a deterministic
hypervisor and explores its behavior under faults. Its great advantage is that you
change nothing. The trade is that it is a service rather than a library you own and
run anywhere. If you cannot restructure your code, or your system spans several
languages and processes, it addresses a case ravel cannot.

**rr.** Records one real execution of a native program, including its nondeterminism,
so you can replay it and debug it backwards. It does not *find* bugs by trying new
executions; it helps you understand one you already have. It pairs well with ravel:
when the code under test is deterministic under ravel, a failing seed is replayable
in any debugger without rr, and rr remains the tool for the nondeterministic bugs
that ravel cannot express.

**Jepsen.** Tests real distributed databases on real clusters with real network
partitions and clock skew, then checks the recorded history against a model such as
linearizability. It finds bugs that live in the real deployment (configuration,
kernel behavior, real timing). Its runs are not repeatable; ravel's are. Use Jepsen
to check the assembled product and ravel to hammer the logic with millions of
repeatable fault schedules.

**Property-based testing.** Generates many inputs for a function and shrinks a
failing one to a minimal example. ravel borrows the idea, in particular the
Hypothesis-style trick of shrinking the *recorded random choices* instead of the
inputs, and applies it to schedules, message loss, delays and crashes, which are
things an ordinary input generator does not control.

## When ravel is the wrong tool

- **You cannot change the code.** A binary you cannot rebuild, a third-party service,
  code in another language: ravel needs a seam. Look at Antithesis or Jepsen-style
  testing.
- **The bug is in real hardware or operating-system behavior**: a specific kernel, a
  real network stack, a particular disk's firmware. ravel models the faults you
  describe, so it finds the bugs those faults reach, not the ones only reality
  produces.
- **The problem is memory ordering between real threads.** Use loom, ThreadSanitizer
  or a model checker.
- **You need performance numbers.** Virtual time is not real time. Simulation tells
  you what happens, not how fast.
- **The code cannot be made deterministic**: it depends on real threads it cannot give
  up, or on hardware it cannot abstract. `check_determinism` will tell you quickly.

## Using ravel with the others

They are complements more than rivals:

- **ravel and Jepsen-style testing.** ravel for the logic under millions of repeatable
  schedules and crashes, and a real-cluster test for what only real deployments show.
- **ravel and sanitizers.** Run the sweep in an ASan/UBSan build now and then: a
  memory error inside a simulated task is a real bug, and a simulation reaches code
  paths ordinary tests do not.
- **ravel and unit tests.** Keep your unit tests. ravel finds the bugs that need a
  particular combination of timing and faults, which unit tests written by hand rarely
  cover; unit tests document intended behavior and run in milliseconds.
- **ravel and a debugger.** Every failure is a seed or a short list of numbers you can
  replay under a debugger as often as you like.

If you are unsure whether ravel fits your system, the
[porting guide](porting.md) shows what the change looks like, and
`--check-determinism` on a first try tells you whether your code is a candidate.
