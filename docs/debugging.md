# Debugging guide: reading what ravel tells you

When a run fails, ravel tells you *what* failed, *where*, and gives you the
means to replay it. This page shows what each kind of problem looks like, with
real output, and what to do about it.

- [Anatomy of a result](#anatomy-of-a-result)
- [What each failure looks like](#what-each-failure-looks-like)
- [Reading a trace](#reading-a-trace)
- [Replaying a failure](#replaying-a-failure)
- [Shrinking: what the numbers mean](#shrinking-what-the-numbers-mean)
- [Questions you will have](#questions-you-will-have)

## Anatomy of a result

Every run produces a `ravel::Result`:

| Field | Meaning |
|---|---|
| `ok` | true if nothing failed |
| `failure` | what went wrong, in a sentence; empty if `ok` |
| `seed` | the seed of the run |
| `steps` | how many times the scheduler resumed a task |
| `trace_digest` | a fingerprint of the whole run; equal digests mean identical runs |
| `trace_path` | the trace file, if `trace_dir` was set and the run failed |

The `failure` sentence is the first thing to read. It always starts with what
kind of thing happened, as the next section shows.

## What each failure looks like

### An invariant failed

```
invariant 'deposit_applied_at_most_once' failed
```

The system ran to the end and a property you stated was false. This is the
failure you are looking for. The [tutorial](tutorial.md#4-hunt-for-the-bug)
walks through one, including how to read the trace that explains it. Invariants
are checked once, when the run ends (or at `time_limit`). To catch a violation
*as it happens*, record it in shared state from the code that notices it, as the
Raft example does, and check that state in the invariant.

### A task threw

A task let an exception escape, which stops the run at that step:

<!-- snippet: docs/snippets/debugging_examples.cpp#task_threw -->
```cpp
// A task that hits a bug and lets an exception escape.
void task_threw() {
  ravel::Simulation sim(1);
  sim.scheduler().spawn("account_service", []() -> ravel::Task {
    throw std::runtime_error("no such account: 42");
    co_return;
  });
  print_result(sim.run_until_quiescent());
}
```

<!-- output: debugging_examples task-threw -->
```text
ok: no
failure: task 'account_service' threw: no such account: 42
steps: 1
seed: 1
```

`steps: 1` says it happened on the very first step. The message names the task
and carries the exception's `what()`. If your task catches everything it will
never show up here; let unexpected exceptions escape.

### The step limit was reached

<!-- snippet: docs/snippets/debugging_examples.cpp#livelock -->
```cpp
// Two tasks that keep yielding to each other and never finish.
void livelock() {
  ravel::Simulation sim(1, ravel::SimulationOptions{.max_steps = 200});
  for (const char* name : {"ping", "pong"}) {
    sim.scheduler().spawn(name, [&sim]() -> ravel::Task {
      while (true) co_await sim.scheduler().yield();
    });
  }
  print_result(sim.run_until_quiescent());
}
```

<!-- output: debugging_examples livelock -->
```text
ok: no
failure: step limit of 200 reached (tasks are still runnable; possible livelock)
steps: 200
seed: 1
```

Tasks were still runnable after 200 steps (the limit was lowered here; the
default is a million). That is a **livelock**: something that keeps running
without making progress, like two tasks that keep yielding to each other, or a
retry loop with no backoff and no limit. Look at the trace's last events to see
who was running. The limit exists so a livelocked system fails the run instead
of hanging your CI. If your system is legitimately busy for longer, raise
`max_steps`.

If instead the system *never goes quiet by design* (heartbeats, election
timers), that is not a livelock: set `SimulationOptions::time_limit` so the
run stops at a virtual time instead, and check your invariants then.

### Everything finished but nothing happened

<!-- snippet: docs/snippets/debugging_examples.cpp#stuck -->
```cpp
// The run "completes", because nothing is left to happen, yet nothing useful
// did: the client waits for a reply from a server that was never started.
void stuck() {
  ravel::Simulation sim(1);
  bool got_reply = false;
  ravel::Channel& to_server = sim.add_channel("client", "server", {});
  ravel::Channel& to_client = sim.add_channel("server", "client", {});
  sim.scheduler().spawn("client", [&]() -> ravel::Task {
    to_server.send("hello");
    co_await to_client.receive();  // Blocks forever.
    got_reply = true;
  });
  sim.add_invariant("client_got_a_reply", [&got_reply] { return got_reply; });
  print_result(sim.run_until_quiescent());
}
```

<!-- output: debugging_examples stuck -->
```text
ok: no
failure: invariant 'client_got_a_reply' failed
steps: 1
seed: 1
```

A run **completes** when nothing is left to happen, and a task waiting for a
message that will never arrive does not stop that. So a system that quietly
deadlocks looks like a passing run, unless an invariant says what should have
happened. Always include a *liveness-flavored* invariant such as "the client got
its reply" or "the write was acknowledged", not only "nothing bad happened".

### ravel refuses to shrink

<!-- snippet: docs/snippets/debugging_examples.cpp#not_reproducible -->
```cpp
// Asking to shrink a failure that does not happen the same way twice.
void not_reproducible() {
  int runs = 0;
  const auto flaky = [&runs](ravel::Simulation& sim) {
    const bool fail = ++runs == 1;  // Fails the first time only.
    sim.add_invariant("flaky", [fail] { return !fail; });
  };
  try {
    ravel::shrink(flaky, /*seed=*/7);
  } catch (const std::exception& e) {
    std::printf("shrink threw: %s\n", e.what());
  }
}
```

<!-- output: debugging_examples not-reproducible -->
```text
shrink threw: ravel::shrink: replaying seed 7 did not reproduce its failure; the setup is not deterministic
```

Shrinking works by replaying the failure over and over, so the failure has to
replay. This message means it did not: the same seed *and the same recorded
choices* failed the first time and not the second. Something outside ravel
influences the code under test. Run `ravel::check_determinism` on your setup;
it will name the step where two runs part ways. The usual culprits are a real
clock, a static that survives between runs, or a container ordered by pointer.
See [the checklist](porting.md#is-it-really-deterministic).

### A determinism problem

```
seed 0: two runs of the same seed diverged at step 2: run 1 TaskResumed 'worker' at t=10, run 2 TaskResumed 'worker' at t=5
```

That is `check_determinism` speaking. It shows two runs of the same seed and the
first event where they differ (here, a task woke at different virtual times).
The [porting guide](porting.md#is-it-really-deterministic) has the full
example and the list of usual causes. There are three phrasings:

- `two runs of the same seed diverged at step N: ...`: the code under test is not
  repeatable.
- `replaying run 1's recorded choices diverged ...`: something other than
  ravel's random choices steers the run, so a saved `.choices` file would not
  reproduce it.
- `the runs made the same moves but ... ended with ...`: the event sequence is
  identical but an invariant reads something that differs between runs.

## Reading a trace

Set `options.simulation.trace_dir = "ravel-traces"` and every failed run leaves a
file there: `ravel-seed-<seed>.trace.jsonl`. It has one line of JSON per event.
Here is the whole trace of a tiny run (a message, then a disk write and sync):

<!-- snippet: docs/snippets/debugging_examples.cpp#trace -->
```cpp
// A message, then a disk write and sync: the whole trace, one event per line.
void trace() {
  ravel::Simulation sim(3);
  ravel::Channel& link = sim.add_channel("client", "server", {.latency_min = 2, .latency_max = 5});
  ravel::Disk& disk = sim.add_disk("ssd", {.latency_min = 1, .latency_max = 3});
  sim.scheduler().spawn("client", [&]() -> ravel::Task {
    link.send("hello");
    co_await disk.write("log", 0, "hello");
    co_await disk.sync("log");
  });
  sim.scheduler().spawn("server", [&]() -> ravel::Task { co_await link.receive(); });
  sim.run_until_quiescent();

  std::ostringstream text;
  sim.write_trace(text);
  std::string trace_text = text.str();
  const std::string version = ravel::version_string();
  trace_text.replace(trace_text.find(version), version.size(), "x.y.z");  // Keep the docs stable.
  std::fputs(trace_text.c_str(), stdout);
}
```

<!-- output: debugging_examples trace -->
```text
{"format":"ravel-trace","trace_version":1,"ravel_version":"x.y.z","seed":3}
{"step":0,"time":0,"kind":"TaskSpawned","id":0,"name":"client"}
{"step":1,"time":0,"kind":"TaskSpawned","id":1,"name":"server"}
{"step":2,"time":0,"kind":"TaskResumed","id":0,"name":"client"}
{"step":3,"time":0,"kind":"MessageSent","id":0,"name":"client->server"}
{"step":4,"time":0,"kind":"TaskResumed","id":1,"name":"server"}
{"step":5,"time":3,"kind":"DiskWritten","id":0,"name":"ssd"}
{"step":6,"time":3,"kind":"TaskResumed","id":0,"name":"client"}
{"step":7,"time":4,"kind":"MessageDelivered","id":0,"name":"client->server"}
{"step":8,"time":4,"kind":"TaskResumed","id":1,"name":"server"}
{"step":9,"time":4,"kind":"TaskFinished","id":1,"name":"server"}
{"step":10,"time":5,"kind":"DiskSynced","id":0,"name":"ssd"}
{"step":11,"time":5,"kind":"TaskResumed","id":0,"name":"client"}
{"step":12,"time":5,"kind":"TaskFinished","id":0,"name":"client"}
```

| Field | Meaning |
|---|---|
| `step` | position in the run, from 0 |
| `time` | virtual time in ticks (never wall-clock time) |
| `kind` | what happened; see the table below |
| `id` | the task, channel or disk it concerns |
| `name` | that thing's name: a task's name, `from->to` for a channel, a disk's name |

The events you will meet most:

| Kind | Reads as |
|---|---|
| `TaskSpawned` | the task was created |
| `TaskResumed` | the scheduler chose this task and ran it to its next `co_await` |
| `TaskFinished` / `TaskThrew` | it returned, or an exception escaped (the run stops) |
| `MessageSent` | someone called `send()` |
| `MessageDropped` | the fault spec lost it |
| `MessageDelivered` | it reached the receiver's inbox |
| `DiskWritten` | a write, rename or removal completed |
| `DiskSynced` | a `sync` or `sync_dir` completed |
| `DiskFailed` | an injected disk error, or out of space |
| `DiskCrashed` | `crash()` was called |

The full field reference is in [formats.md](formats.md).

Reading the example above: at `t=0` the client sends a message and starts a disk
write; the disk takes 3 ticks (`DiskWritten` at `t=3`) while the message is in
flight for 4 (`MessageDelivered` at `t=4`); the server wakes up, finishes; the
`sync` completes at `t=5`. Notice how *the order of things is visible*: the write
finished before the message arrived. If a bug depends on that order, you can
see it here.

### Reading traces with ravel_trace

A raw trace is easy for a program and tiring for a person. `tools/ravel_trace.py`
(plain Python 3, no dependencies) turns it into something you can read at a glance.
The examples use the tutorial's two traces for the failing deposit: the original
failing run, and the minimal one ravel shrank it to.

**`summary`**: what is in the trace, and anything that deserves a look.

<!-- tool: ravel_trace.py summary docs/snippets/data/deposit_original.trace.jsonl -->
```text
docs/snippets/data/deposit_original.trace.jsonl
seed 6, ravel x.y.z
26 events over 208 ticks of virtual time

events by kind:
  kind              count
  MessageDelivered  3
  MessageDropped    4
  MessageSent       7
  TaskFinished      1
  TaskResumed       9
  TaskSpawned       2

events by task, channel or disk:
  name            count
  client          8
  client->server  10
  server          4
  server->client  4

worth a look:
  4 x message(s) lost to the fault spec
```

Four lost messages, and (from the tutorial) we know that one lost *reply* is the
whole bug.

**`timeline`**: one column per task, channel and disk, one row per event, in time
order. This is usually the fastest way to *see* a failure. Words in capitals mark
what deserves a look (`DROPPED`, `CRASH`, `FAILED`, `THREW`):

<!-- tool: ravel_trace.py timeline docs/snippets/data/deposit_minimal.trace.jsonl -->
```text
time  step  server   client    client->server  server->client
0     0     spawned
0     1              spawned
0     2     runs
0     3              runs
0     4                        send
1     5                        deliver
1     6     runs
1     7                                        send
1     8                                        DROPPED
50    9              runs
50    10                       send
51    11                       deliver
51    12    runs
51    13                                       send
52    14                                       deliver
52    15             runs
52    16             finished
```

Read it like a sequence diagram: the request goes out (`send`, then `deliver`), the
server runs, its reply is `DROPPED`, and fifty ticks later the client runs again and
sends the request a second time.

**`show`**: the events as a plain table, with filters `--name` (a task, channel or
disk), `--kind`, `--from` and `--to` (virtual time). For example, only what was
lost:

<!-- tool: ravel_trace.py show docs/snippets/data/deposit_original.trace.jsonl --kind MessageDropped -->
```text
step  time  kind            name
8     1     MessageDropped  server->client
11    50    MessageDropped  client->server
14    100   MessageDropped  client->server
17    150   MessageDropped  client->server
```

**`diff`**: where two traces part ways. Point it at the original failing run and the
minimal one to see what shrinking threw away:

<!-- tool: ravel_trace.py diff docs/snippets/data/deposit_original.trace.jsonl docs/snippets/data/deposit_minimal.trace.jsonl -->
```text
the traces agree for 2 events, then part ways at step 2:
   step  A: deposit_original.trace.jsonl  B: deposit_minimal.trace.jsonl
   0     t=0 TaskSpawned server           t=0 TaskSpawned server
   1     t=0 TaskSpawned client           t=0 TaskSpawned client
>  2     t=0 TaskResumed client           t=0 TaskResumed server
   3     t=0 MessageSent client->server   t=0 TaskResumed client
   4     t=0 TaskResumed server           t=0 MessageSent client->server

A has 26 events, B has 17.

what differs, by kind:
  kind            A  B  change
  MessageDropped  4  1  -3
  MessageSent     7  4  -3
  TaskResumed     9  6  -3
```

The runs part at step 2 (the original started the client first; the minimal run
starts the server), and the table at the bottom is the summary of what shrinking
removed: three of the four lost messages and the retries they caused. `diff`
exits with status 1 when the traces differ and 0 when they are identical, so it also
works in scripts, for instance to check that two runs of the same seed match.

### Slicing traces with jq

Traces are plain JSON Lines, so ordinary tools work. With
[jq](https://jqlang.github.io/jq/) (the outputs below are from the tutorial's
trace, `ravel-seed-6.replay.trace.jsonl`):

Only the lost messages:

```
$ jq -c 'select(.kind == "MessageDropped")' ravel-seed-6.replay.trace.jsonl
{"step":8,"time":1,"kind":"MessageDropped","id":1,"name":"server->client"}
```

One event per line, as a table:

```
$ jq -r '[.step, .time, .kind, .name] | @tsv' ravel-seed-6.replay.trace.jsonl | tail -4
13	51	MessageSent	server->client
14	52	MessageDelivered	server->client
15	52	TaskResumed	client
16	52	TaskFinished	client
```

Everything one task did:

```
$ jq -c 'select(.name == "client")' ravel-seed-6.replay.trace.jsonl | head -3
{"step":1,"time":0,"kind":"TaskSpawned","id":1,"name":"client"}
{"step":3,"time":0,"kind":"TaskResumed","id":1,"name":"client"}
{"step":9,"time":50,"kind":"TaskResumed","id":1,"name":"client"}
```

What happened between two moments in time:

```
$ jq -c 'select(.time >= 50 and .time <= 51)' ravel-seed-6.replay.trace.jsonl
```

No `jq`? `grep MessageDropped ravel-seed-6.replay.trace.jsonl` does the first one.

## Replaying a failure

A failing seed is a complete, repeatable description of the run, so you can
watch it as many times as you like.

**From a seed.** Build the simulation yourself and run it in a debugger:

```cpp
ravel::Simulation sim(6);          // the failing seed
my_setup(sim);
ravel::Result result = sim.run_until_quiescent();   // set breakpoints in your tasks
```

Every run of that code takes the same steps, so a breakpoint in a task hits at
the same moment each time, and you can step through the exact interleaving that
failed.

**From a choices file** (no seed needed, and it survives changes to ravel's
random number generator). Use the shrunk one; it is the shortest run that fails:

```cpp
std::ifstream file("ravel-traces/ravel-seed-6.choices");
ravel::Result result = ravel::replay(my_setup, ravel::read_choices(file));
```

Remember to pass the same `SimulationOptions` the failing run used (such as
`time_limit`) as the third argument of `replay` if you set any.

**Print the trace of a run that passed.** Traces are only written for failures,
but `sim.write_trace(std::cout)` works after any run.

## Shrinking: what the numbers mean

```
shrunk from 11 random choices (9 steps) to 4 (6 steps)
minimal choices: 0 0 0 1
```

- *Random choices* are the decisions ravel made in the run: which task next,
  message lost or not, how long a delay, and everything your workload drew from
  `sim.rng()`. Fewer is simpler.
- *Steps* is how many times a task was resumed. It can go **up** while shrinking:
  the minimal run is the one with the fewest and smallest *choices*, not the
  fewest steps. (A lost message, for instance, can trigger retries.)
- Each number is 0 for the simplest outcome (first task, no fault, shortest
  delay). A `1` in a fault position means "the fault happens". So **the nonzero
  numbers in a minimal list are the ingredients the bug needs**; everything else
  is irrelevant. [The tutorial](tutorial.md#6-what-is-a-choice) reads one out
  loud.
- If `ShrinkResult::budget_exhausted` is true, shrinking stopped at
  `max_attempts` (20000 by default) before it finished. Raise it, or accept a
  result that is smaller but maybe not minimal.

## Questions you will have

**My invariant failed on some seeds. Is it a bug in my code or my invariant?**
Shrink it and read the trace: it is the shortest sequence of events that violates
the property. If those events show a real problem, it is your code. If they show
something legitimate (a fault your system is *supposed* to tolerate), your
invariant is too strict.

**A seed fails on my machine and not in CI.** That should not happen: same seed,
same run, everywhere. Run `ravel::check_determinism`. Different results on
different machines means something outside ravel is leaking into the run, most
often an uninitialized variable or a container ordered by pointer.

**The trace is huge.** Failures on long runs are long. Shrink first (the minimal
run is usually short), or filter with `jq`. Tracing costs memory in proportion
to the number of steps, so very long runs are better bounded with `max_steps` or
`time_limit`.

**How many seeds is enough?** Bugs that need one unlucky moment show up on a
fraction of seeds; the tutorial's showed up on 28%. Rarer ones need more: the
Raft example's rarest bug is about 1 seed in 600. Run thousands in CI and more
overnight, since seeds are cheap and each is a new chance.
