# C ABI stability policy

`include/ravel/ravel.h` is the C ABI surface. Until `1.0.0`, it can break
between minor versions — every break must be called out in the release
notes. From `1.0.0` on:

- **Breaking** (major version bump): removing or renaming an exported
  function, changing a function's signature, changing the layout/size of
  any struct passed by value across the boundary (none yet — `ravel_simulation`
  is opaque on purpose, keep it that way rather than exposing a struct
  layout).
- **Non-breaking** (minor version bump): adding a new exported function,
  adding a new opaque handle type.
- **Patch version**: behavior fixes that don't change the signature surface.

Keep every ABI type opaque (`ravel_simulation*`, not a struct with public
fields) specifically so internal layout can change without an ABI break —
this is why `ravel_simulation` is a forward-declared pointer, not a struct
the caller allocates.
