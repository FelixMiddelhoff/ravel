# Contributing to ravel

## Building and testing

```bash
cmake -S . -B build -DRAVEL_SHARED=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Format before committing:

```bash
clang-format -i include/ravel/*.hpp src/*.cpp tests/*.[ch]pp examples/*.cpp
```

`.clang-format` and `.clang-tidy` at the repo root pin the style/lint rules;
most editors pick them up automatically.

## Documentation

The tutorial, porting guide and debugging guide quote real code and real
program output from `docs/snippets`. After changing either, refresh the docs
and review the diff:

```bash
cmake --build build
python3 tools/check_docs.py --build-dir build --update
```

CI runs the same script without `--update` and fails if the docs are out of
date, so the documentation cannot drift from what the code does.

## The property that must never regress

`VirtualRng` and `VirtualClock` being seed-deterministic — same seed, same
output, on every platform — is the one guarantee the entire library exists
to provide. Any change touching `include/ravel/rng.hpp`, `clock.hpp`, or
`src/scheduler.cpp`'s interleaving logic needs a test proving determinism
still holds, not just that the change compiles.

## Submitting a pull request

1. Open an issue first for anything non-trivial (a new fault-injection
   type, a change to the scheduler's interleaving algorithm, a new C ABI
   function) — cheap to discuss before code exists.
2. Keep PRs scoped to one change.
3. CI must pass (build matrix + sanitizers) before merge.
4. New public API (`include/ravel/`) needs a test and a README update in
   the same PR: docs and code change together, not in a follow-up.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
