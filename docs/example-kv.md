# Example: a key-value store that survives a power cut

The [tutorial](tutorial.md) tested a network protocol. This example tests
**storage**: a tiny key-value store that keeps its data in a write-ahead log, and
what happens to it when the power fails. Disks are where the subtle bugs hide,
because a disk lies in ways a network does not: it tells you a write is done when
the data is still in memory, and a power cut can lose it, or tear it in half.

Two bugs to find, both real ones:

1. **Acknowledging before syncing.** The server tells the client "stored" while
   the data is still in the disk's cache.
2. **Trusting a torn record.** After a crash, recovery believes a log record that
   was cut off halfway.

Every code block and every output block below is produced by the real programs
in [`docs/snippets`](snippets); `tools/check_docs.py` keeps them honest.

- [The store](#the-store)
- [What "correct" means](#what-correct-means)
- [Run it](#run-it)
- [Bug 1: acknowledging before syncing](#bug-1-acknowledging-before-syncing)
- [Bug 2: trusting a torn record](#bug-2-trusting-a-torn-record)
- [What to take away](#what-to-take-away)
- [Try it yourself](#try-it-yourself)

## The store

The server appends every put to a log file, `wal`, as one line, `PUT <key> <value>`,
and tells the client the put is stored. After a crash, the machine reboots and
**recovers** by replaying the log from the start.

The server, one put at a time:

<!-- snippet: docs/snippets/kv_store.hpp#kv_server -->
```cpp
    // The server appends each put to the log and tells the client it is stored.
    sim.scheduler().spawn("server", [&sim, &state, &disk, bug]() -> ravel::Task {
      const std::uint64_t boot = disk.crash_count();  // If the power goes, this process is gone.
      const auto alive = [&disk, boot] { return disk.crash_count() == boot; };
      const auto ok = [&alive](ravel::DiskStatus status) {
        return status == ravel::DiskStatus::Ok && alive();
      };

      std::uint64_t log_size = 0;
      for (int version = 1; version <= kPuts; ++version) {
        const std::string key = "k" + std::to_string(version % kKeys);
        const std::string record = "PUT " + key + " " + value_for(version) + "\n";

        if (!ok(co_await disk.write("wal", log_size, record))) co_return;
        log_size += record.size();

        if (bug != Bug::AckBeforeSync && !ok(co_await disk.sync("wal"))) co_return;
        state.acknowledged[key] = version;  // "Stored": the client may rely on this.
        if (bug == Bug::AckBeforeSync && !ok(co_await disk.sync("wal"))) co_return;
      }
    });
```

The order of the three things inside the loop is the whole story. In the correct
version: **write** the record, **sync** the file (which forces it onto the disk),
and only then **acknowledge**. A record is *durable* only after `sync`; before
that it lives in the disk's cache, and a power cut may lose it or tear it (write
only the first sector or two).

Notice `alive()`. When the power goes, this process is gone: it must stop, even if
its last disk operation completed a moment earlier. (The
[porting guide](porting.md#pitfalls-with-coroutines) explains why.)

Then a second task pulls the plug at a random moment and, a few ticks later,
reboots and recovers:

<!-- snippet: docs/snippets/kv_store.hpp#kv_recovery -->
```cpp
    // The power fails at some point; a moment later the machine boots and
    // rebuilds the store from whatever is in the log.
    sim.scheduler().spawn("power_cut", [&sim, &state, &disk, bug]() -> ravel::Task {
      co_await sim.scheduler().sleep(sim.rng().next_between(20, 120));
      disk.crash();
      co_await sim.scheduler().sleep(10);
      const ravel::ReadResult log = co_await disk.read("wal", 0, 1 << 20);
      state.recovered = replay_log(log.data, /*trust_partial_tail=*/bug == Bug::TrustPartialTail);
      state.recovery_done = true;
    });
```

and recovery itself is a loop over the log:

<!-- snippet: docs/snippets/kv_store.hpp#kv_replay -->
```cpp
// Rebuilds the key-value map from the log: replay every record, in order.
inline std::map<std::string, std::string> replay_log(const std::string& log, bool trust_partial_tail) {
  std::map<std::string, std::string> store;
  std::size_t start = 0;
  while (start < log.size()) {
    const std::size_t end = log.find('\n', start);
    const bool complete = end != std::string::npos;
    if (!complete && !trust_partial_tail) break;  // A record cut off by the crash: ignore it.

    const std::string line = log.substr(start, (complete ? end : log.size()) - start);
    const std::size_t key_end = line.find(' ', 4);  // "PUT <key> <value>"
    if (line.rfind("PUT ", 0) == 0 && key_end != std::string::npos) {
      store[line.substr(4, key_end - 4)] = line.substr(key_end + 1);
    }
    if (!complete) break;
    start = end + 1;
  }
  return store;
}
```

A crash can leave the *last* record cut off (it has no closing newline), so
recovery must ignore a trailing fragment. That is the `if (!complete ...) break`
line, and it is bug 2's hiding place.

The values are 600 bytes long on purpose: a disk sector is 512 bytes, so each record
spans two sectors, which is what lets a torn write cut one in the middle.

## What "correct" means

Two properties, stated as invariants:

<!-- snippet: docs/snippets/kv_store.hpp#kv_invariants -->
```cpp
    // Recovery must not invent data: every value it finds is one that was written.
    sim.add_invariant("recovery_invents_nothing", [&state] {
      for (const auto& [key, value] : state.recovered) {
        const int version = version_of(value);
        if (version < 1 || version > kPuts || "k" + std::to_string(version % kKeys) != key) return false;
      }
      return true;
    });

    // Once the client was told "stored", the data must survive the crash.
    sim.add_invariant("acknowledged_puts_survive", [&state] {
      if (!state.recovery_done) return false;
      for (const auto& [key, version] : state.acknowledged) {
        const auto found = state.recovered.find(key);
        if (found == state.recovered.end() || version_of(found->second) < version) return false;
      }
      return true;
    });
```

- **`acknowledged_puts_survive`**: if the client was told "stored", the value must be
  there after the crash. That is what "stored" *means*.
- **`recovery_invents_nothing`**: everything recovery finds must be something that
  was actually written. A half-written value is not.

## Run it

`kv_sweep` runs the store under 500 seeds, with a `--bug` flag to pick the
variant. The correct store first:

<!-- output: kv_sweep -->
```text
ok: 500 seeds passed (seeds 0..499)
```

500 seeds, each with a power cut at a different moment and a different fate for
whatever was unsynced, and no failures. Now the bugs.

## Bug 1: acknowledging before syncing

The tempting optimization: tell the client "stored" as soon as the record is
written, then sync afterwards (or in the background), so the client does not wait
for the disk. It is faster, and it is wrong:

<!-- output: kv_sweep --bug ack-before-sync -->
```text
FAILED: 79 of 500 seeds
first failure: seed 7: invariant 'acknowledged_puts_survive' failed
shrunk from 21 random choices (21 steps) to 8 (11 steps)
reproducer: ravel-traces/ravel-seed-7.choices
replay it:  kv_sweep --replay ravel-traces/ravel-seed-7.choices
trace:      ravel-traces/ravel-seed-7.replay.trace.jsonl
```

About one seed in six fails. The shrunk reproducer is 8 choices; let us read the
run it describes. `ravel_trace.py timeline` (see the
[debugging guide](debugging.md#reading-traces-with-ravel_trace)) lays the trace
out with one column per task and disk:

<!-- tool: ravel_trace.py timeline docs/snippets/data/kv_ack_before_sync.trace.jsonl -->
```text
time  step  server    power_cut  ssd
0     0     spawned
0     1               spawned
0     2     runs
0     3               runs
1     4                          write
1     5     runs
2     6                          sync
2     7     runs
6     8                          write
6     9     runs
11    10                         sync
11    11    runs
16    12                         write
16    13    runs
20    14              runs
20    15                         CRASH
21    16    runs
21    17    finished
30    18              runs
31    19              runs
31    20              finished
```

Read it top to bottom. The server writes put 1 at `t=1` and syncs it at `t=2`; put 2
is written at `t=6` and synced at `t=11`; put 3 is **written at `t=16`**, and the
server (now at step 13) acknowledges it and *starts* its sync, which will take
until `t=21`. But at `t=20` the `power_cut` task pulls the plug: `CRASH` on the
`ssd` column. The sync never finished. Put 3 was acknowledged and is gone.

The 8 choices say the same thing in numbers:

| Choice | Draw | Value | Meaning |
|---|---|---|---|
| 1st | which task runs first | 0 | the server |
| 2nd | write 1's latency | 0 | 1 tick |
| 3rd | when the power fails | 0 | at `t=20`, the earliest possible |
| 4th | sync 1's latency | 0 | 1 tick |
| 5th | write 2's latency | 3 | 4 ticks |
| 6th | sync 2's latency | 4 | 5 ticks |
| 7th | write 3's latency | 4 | 5 ticks |
| 8th | sync 3's latency | 4 | 5 ticks: it would finish at `t=21`, a tick too late |

And after that, the disk's own draw at the crash takes its default, 0: **the
unsynced write is lost**. The bug needs nothing exotic: a slow disk and a power
cut in a five-tick window.

Replay the saved reproducer, which lives in
[`docs/snippets/data`](snippets/data):

<!-- output@root: kv_sweep --bug ack-before-sync --replay docs/snippets/data/kv_ack_before_sync.choices -->
```text
replay of docs/snippets/data/kv_ack_before_sync.choices (8 choices): FAILED: invariant 'acknowledged_puts_survive' failed
```

The fix is to swap the order back: sync, then acknowledge (the store above with
`--bug none`).

## Bug 2: trusting a torn record

The second bug is in recovery. Suppose a power cut hits *while a record is being
written*: the disk may write the first sector and lose the rest, leaving a log that
ends mid-record, with no closing newline. Correct recovery ignores that fragment.
This variant believes it:

<!-- output: kv_sweep --bug trust-partial-tail -->
```text
FAILED: 25 of 500 seeds
first failure: seed 7: invariant 'recovery_invents_nothing' failed
shrunk from 21 random choices (21 steps) to 12 (13 steps)
reproducer: ravel-traces/ravel-seed-7.choices
replay it:  kv_sweep --replay ravel-traces/ravel-seed-7.choices
trace:      ravel-traces/ravel-seed-7.replay.trace.jsonl
```

This time the invariant that breaks is `recovery_invents_nothing`: recovery
invented a value nobody ever wrote (a real value with its tail sliced off), and it
went into the store as if it were the truth. That is a subtler bug than losing
data, because nothing *looks* wrong until you compare the value with what was
written. It also only appears on the seeds where the power cut lands mid-write **and**
the disk tears that write at a sector boundary, which is why it hit 25 seeds out of
500 and not more. Replay its reproducer:

<!-- output@root: kv_sweep --bug trust-partial-tail --replay docs/snippets/data/kv_trust_partial_tail.choices -->
```text
replay of docs/snippets/data/kv_trust_partial_tail.choices (12 choices): FAILED: invariant 'recovery_invents_nothing' failed
```

Ask the correct store the same question, with the same reproducer, and it passes:

<!-- output@root: kv_sweep --replay docs/snippets/data/kv_trust_partial_tail.choices -->
```text
replay of docs/snippets/data/kv_trust_partial_tail.choices (12 choices): ok
```

That is what a saved reproducer is for: it kept the exact moment that broke the
buggy code, and it now guards the fixed code.

## What to take away

- **Order matters, and only a crash reveals it.** "Write, sync, acknowledge" versus
  "write, acknowledge, sync" behaves identically until the power fails, so ordinary
  tests never tell them apart. ravel's disk crashes at random moments and loses
  whatever was not synced.
- **State what "stored" means as an invariant** (`acknowledged_puts_survive`), and
  check it *after* a crash. Half the value of this kind of test is writing that
  sentence down.
- **Recovery is code too**, and it runs in the worst possible conditions. Test it
  with the same care as the happy path.
- **The reproducer is tiny.** Eight numbers describe the whole failure, and the trace
  shows it in seventeen lines. That is what makes a bug that needs "a power cut in
  a five-tick window" a bug you can actually fix.

## Try it yourself

Some experiments, each a few lines in [`kv_store.hpp`](snippets/kv_store.hpp):

- **Add a hole.** Let two puts be in flight at once (write both, then sync). A crash
  can now lose the *first* and keep the *second*, leaving zero bytes in the middle of
  the log. Does recovery cope?
- **Forget the directory.** Have recovery start from a `wal.new` file that is renamed
  over `wal` at checkpoints, and leave out `sync_dir`. (The
  [`disk` tests](../tests/test_disk_names.cpp) show the safe recipe.)
- **Add a checksum** to each record and make recovery stop at the first bad one. The
  torn-record bug goes away for a better reason than "ignore the tail".
- **Turn on write errors**: `{.write_error_probability = 0.05}` in the disk's fault
  spec. What should the server do when a write fails? Does your answer survive the
  sweep?

For a bigger system that combines all of this (crashing nodes, disks, a lossy
network), read the [Raft example](../examples/raft.hpp).
