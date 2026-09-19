# File and digest formats

ravel writes two kinds of files, and computes one digest. All three are meant
to be stable enough to build tooling on. Each carries a version so a reader
can tell whether it understands what it is looking at.

## Trace file: `ravel-seed-<seed>.trace.jsonl`

Written when a run fails and `SimulationOptions::trace_dir` is set. A run
replayed from a choice list writes `ravel-seed-<seed>.replay.trace.jsonl`
instead, so the original and the minimal run sit side by side.

The file is [JSON Lines](https://jsonlines.org): one JSON object per line,
UTF-8, `\n` line endings.

**Line 1, the header:**

```json
{"format":"ravel-trace","trace_version":1,"ravel_version":"0.1.0","seed":151}
```

| Field | Meaning |
|---|---|
| `format` | Always `"ravel-trace"`. |
| `trace_version` | Version of this format. Readers must reject a version they do not know. |
| `ravel_version` | The ravel that wrote the file. |
| `seed` | The seed of the run (`0` for a run replayed from a choice list). |

**Every other line is one event**, in the order it happened:

```json
{"step":4,"time":10,"kind":"MessageDelivered","id":0,"name":"client->replica0"}
```

| Field | Meaning |
|---|---|
| `step` | Position in the trace, counting from 0. |
| `time` | Virtual time (ticks) when it happened. Never wall-clock time. |
| `kind` | One of the kinds below. |
| `id` | Id of the task, channel or disk the event concerns, in the order they were created (from 0). |
| `name` | That subject's name: the task's name, `from->to` for a channel, the disk's name. |

| `kind` | Subject | Meaning |
|---|---|---|
| `TaskSpawned` | task | The task was registered. |
| `TaskResumed` | task | The scheduler picked it and ran it to its next suspension. |
| `TaskFinished` | task | It ran to completion. |
| `TaskThrew` | task | An exception escaped it; the run stops here. |
| `MessageSent` | channel | `send()` was called. |
| `MessageDropped` | channel | The message was lost to the fault spec. |
| `MessageDelivered` | channel | The message reached the receiver's inbox. |
| `DiskWritten` | disk | A write completed and is visible to reads. |
| `DiskSynced` | disk | A sync completed; covered writes are durable. |
| `DiskFailed` | disk | A write or sync failed (no space, or an injected error). |
| `DiskCrashed` | disk | `crash()` was called. |

New event kinds may be added in a minor version without changing
`trace_version`, so readers should skip a `kind` they do not know. Removing or
renaming a field, or changing what one means, bumps `trace_version`.

The trace does not record random draws or the scheduler's picks as separate
events. They are implied by the order of `TaskResumed` events, and recorded in
full in the choice list below.

## Choice list: `ravel-seed-<seed>.choices`

Written by `shrink()` (and so by `run_seeds` with `shrink_first_failure`) next
to the traces. It is the complete description of a run: every random decision,
in order.

```
ravel-choices 1
4
0 1 0 2
```

1. The line `ravel-choices 1`: the format name and version.
2. The number of values, in decimal.
3. The values: unsigned decimal integers separated by whitespace (ravel writes
   16 to a line).

Readers must reject a different version, a missing header, a value that is not
an unsigned 64-bit integer, and a file with fewer values than it promised.
Extra values after the counted ones are ignored.

**Meaning of a value.** Each value answers one random draw, in the order the
draws happen. A draw has a bound, and 0 is always its simplest outcome:

- picking among `n` runnable tasks: an index in `[0, n)`, with 0 the task that
  has been runnable longest;
- a fault that may or may not happen (message loss): 0 = no, 1 = yes;
- a delay in `[min, max]`: an offset from `min`;
- a disk crash: first, how many of the pending directory changes (file
  creations, renames, removals) survive, in order: 0 = none, up to the number
  pending; then, for each unsynced write of each surviving file, its fate: 0 =
  lost, 1 = torn (the next value says how many sectors survived), 2 = survives
  whole.

A value larger than its draw's bound is clamped to `bound - 1`. Draws past the
end of the list get 0. So **every list is a valid run**, which is what lets
shrinking edit lists freely, and lets a saved list keep working after the
system under test changes shape (it may just stop failing).

A choice list depends on the *structure* of the run (which draws happen, in
what order), not on the seed or on ravel's random number generator. It stays
valid across changes to the generator, but not across changes to the code under
test or to how ravel consumes randomness. Pre-1.0 the latter can change in any
minor version.

## Trace digest

`Result::trace_digest` is a 64-bit fingerprint of everything the trace
records. Two runs have equal digests exactly when they made the same
scheduling decisions and had the same message and disk fates at the same
virtual times. It is how replay is checked.

It is FNV-1a over the events in order. For each event, the three values
`time`, `subject` (the numeric `id`) and `kind` (its index in the table above,
starting at 0) are each fed as 8 bytes, least significant byte first:

```
digest = 0xCBF29CE484222325
for each byte b:  digest = (digest XOR b) * 0x100000001B3   (mod 2^64)
```

Because bytes are fed in a fixed order, the digest is the same on every
platform, regardless of endianness. Golden tests in `tests/` pin it.
