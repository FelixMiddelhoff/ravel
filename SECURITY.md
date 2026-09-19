# Security policy

## Reporting a vulnerability

Please **do not** open a public issue for a security vulnerability. Use
GitHub's private reporting instead:

1. Go to the [Security tab](https://github.com/FelixMiddelhoff/ravel/security).
2. Click **Report a vulnerability**.
3. Describe the issue and, if possible, how to reproduce it.

## Supported versions

ravel is pre-1.0 and tracks a single moving line: the latest commit on
`main`. There is no older release branch receiving backports.

## Scope

ravel is a testing/simulation harness, not a production runtime component —
it links into test binaries, not into the system under test's shipped
artifact. Realistic concerns:

- Out-of-bounds access, integer overflow, or UB reachable from public API
  inputs (seeds, fault specs, channel names a caller controls).
- The C ABI (`include/ravel/ravel.h`) never letting a C++ exception cross
  into a C caller (undefined behavior on ABI boundaries) — every function
  there must catch internally and return a status code instead.
- `VirtualRng` (`include/ravel/rng.hpp`) is **not cryptographically secure**
  and must never be used as a source of randomness for anything
  security-sensitive (tokens, keys, nonces) in code that happens to also use
  ravel for testing. It is a fast, portable, reproducible PRNG only.

## Misuse boundaries

- **Fault injection is opt-in and local.** `FaultSpec` on a `Channel` only
  affects messages routed through ravel's own virtual `Channel`/network
  types inside a `Simulation` process — it has no ability to reach a real
  socket, a real disk, or a process ravel didn't spawn. There is no "target
  a remote host" mode; the library has no code path that opens a real
  network connection.
- **No dynamic code loading, no eval of external input.** Simulation
  scenarios are C++ code the caller compiles and links, not a scripting
  format ravel parses — so there is no scenario-file injection surface to
  worry about.
- If a future version adds real syscall/process virtualization (v1 stretch
  scope), that boundary gets its own security-review pass and its own entry
  here before it ships — it is not part of the v0.1 surface today.
