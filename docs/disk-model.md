# The disk model

`ravel::Disk` is a model, not a filesystem. This page states exactly what it
promises, in the order things happen, so you can tell whether a bug it finds (or
fails to find) says something about a real system. It also says where the model
is stricter or looser than the filesystems people actually run, and how the model
itself is tested.

For the API see the `Disk` reference; for a worked example see
[the key-value store walkthrough](example-kv.md).

## The rules

### Files and reads

- Paths use `/`, have no leading slash, and name files. Directories exist only
  as long as a file is under them; `""` is the root.
- A write is visible to every later read as soon as it **completes** (each
  operation takes virtual time). Reads see durable bytes with every completed,
  unsynced write laid over them, in completion order.
- A write past the end of a file grows it with zero bytes. A zero-length write
  past the end also grows the file.
- Writing to a missing file creates it. The creation is a directory change (see
  below).
- Reading past the end returns nothing (status `Ok`); reading a missing file is
  `NotFound`.
- The total size of all files is capped by `capacity_bytes`. A write that would
  exceed it, or whose end offset overflows 64 bits, fails with `NoSpace` and
  changes nothing. Overwriting existing bytes never needs new space.

### Making things durable

- **`sync(path)`** makes durable every write to that file that completed before
  the sync was issued. Writes that complete while the sync is in flight are not
  covered. It also makes the file's creation durable, and, because directory
  changes reach the disk in order, every directory change made before that
  creation. It does not make a later rename or removal durable.
- **`sync_dir(dir)`** makes durable every directory change that completed before
  the call was issued, up to the last one that touched `dir`. A create or remove
  touches the directory of its path; a rename touches the directories of both
  its old and new name. It does not make any file's *data* durable. Syncing a
  directory that does not exist is `Ok`.
- Directory changes (create, rename, remove) are visible at once. A rename
  replaces an existing target. Renaming a file onto itself is `Ok` and does
  nothing.

### Faults

- `write_error_probability`: a write, rename or remove fails with `IoError` and
  changes nothing. The decision is drawn **before** the file is looked up, so a
  rename of a missing file can report `IoError` rather than `NotFound`.
- `sync_error_probability`: `sync(path)` fails with `IoError` **after** the
  missing-file check. The writes it should have flushed are dropped: they vanish
  from reads too, as when an operating system discards dirty pages after a
  failed write-back. The file's creation is not made durable. `sync_dir` fails
  with `IoError` and changes nothing.

### Crashes

`crash()` is instant and does this, in order:

1. Directory changes that were not durable persist as a prefix of the pending
   ones: the number kept is drawn uniformly from `0` to all of them. The
   simplest outcome (what shrinking steers toward) is that none persist.
2. Files whose names are not durable now disappear with their data.
3. For each remaining file, oldest first, each unsynced write, in completion
   order, independently:
   - is lost (the simplest outcome),
   - survives whole, or
   - is torn: a whole number of 512-byte sectors, at least none and fewer than
     all, is applied. A torn write that keeps no sectors is lost.

   Zero-length writes are skipped.
4. Reads and listings now see exactly the durable state. Every operation still in
   flight fails with `Crashed`.

Because unsynced writes survive independently, a crash can keep a later write
and lose an earlier one to the same file.

### The order of random decisions

For a replay to work, the decisions are drawn in a fixed order, which is part
of the model: the latency of an operation when it is issued; then, when it
completes, the error decision (as above); for a crash, the prefix length, then
per file and write the fate and, if torn, the number of sectors.

## Against real filesystems

The model is written from how POSIX filesystems commonly behave and is **not**
validated against one: nothing here was checked by cutting power to real
hardware. Treat this section as intent.

Where it matches the common picture:

- Data is not durable until `fsync`. Directory entries are not durable until the
  directory is synced, and `fsync` on a new file also persists its entry on
  ext4 and xfs.
- A rename is not durable until the directory is synced, so the classic safe
  replace (write a temporary file, sync it, rename over the target, sync the
  directory) is the sequence that survives every crash the model can produce.
- A failed sync can drop data that a later sync will not resurrect.

Where the model is **stricter** than what a filesystem may do (a bug hidden by
this can appear in production):

- Directory changes always persist as an in-order prefix. POSIX promises much
  less, and some filesystems reorder metadata.
- A crash keeps whole 512-byte sectors of a write. Real devices may tear at
  other sizes or leave garbage in a sector.

Where it is **looser** (a bug reported here may be impossible on a given
filesystem):

- Unsynced writes to one file survive independently in any combination. With
  ext4's ordered mode, and with filesystems that journal data, far fewer
  outcomes occur.
- ext4 has a heuristic that flushes data when a file is renamed over an existing
  one (`auto_da_alloc`); the model does not, and will report data loss ext4 may
  hide.

Not modeled at all: truncate, append, hard links, symlinks, permissions, time
stamps, separate directory creation and removal, bit rot, misdirected writes,
disk-full while syncing, and read errors.

## How the model is tested

`tests/disk_model.hpp` holds a second implementation of these rules, written
differently from `src/disk.cpp` (it derives visible names by replaying a log of
pending directory changes, and rebuilds file contents from stable bytes plus
unsynced writes on every access). `ravel_tests` runs 3000 random sequences of 40
operations on both and compares every result, read and listing, and after every
step each file's size, the durable names and the durable contents. `ravel_soak`
runs the same comparison for as many sequences as you ask for; a million pass.

What this does not check: operations that overlap in time (a sync racing a
write), which are covered by hand-written tests, and the order of random draws,
which the model shares with the disk by design (see above). If the model and the
disk disagree, either may be wrong; the discrepancy is settled against these
rules.
