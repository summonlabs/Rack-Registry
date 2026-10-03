# Contributing to Rack Registry

Thank you for your interest in contributing to Rack Registry. This document
describes the conventions used in this repository so that every contribution
lands cleanly.

## Scope

Rack Registry is the canonical rack-composition runtime of the Data Center
Control Plane (DCCP), Tranche 1. It answers:

* Which racks exist, and what is each rack's physical unit extent?
* Which members occupy which mounting coordinates in each rack, and which
  generation of that composition is authoritative?
* Which power and cooling domains is a rack associated with?
* Is a proposed member placement compatible with the rack's declared profile,
  and does it overlap existing occupancy?
* Which lifecycle state is a rack in, and which mutations does that state gate?

Rack Registry deliberately does **not** become a Facility Topology, a Physical
Location Registry, an Asset Registry, a Facility Dependency Registry, a
Facility Capacity Reservation, a Facility State Ledger, or a Failure Domain
Registry. It also does not perform electrical or cooling actuation, placement
optimization, or capacity planning. Contributions that pull those boundaries
into Rack Registry will be redirected.

## Development setup

Rack Registry targets:

* C++20 (portable, standard library only, no third-party dependencies)
* Windows / MSVC 19.44 with CMake and Ninja, or GCC/Clang with CMake
* A local filesystem as the durable substrate for authoritative state

Clone the repository, then:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite must be allowed to run to completion. It contains no timeouts
and no watchdog-success logic by design: a hanging test is a defect to diagnose
and repair, not something to mask.

## Coding guidelines

* Use the `rackregistry` namespace.
* Use strong types for identities, generations, revisions and epochs. Never
  pass a raw integer where a `RackGeneration`, `RackRevision` or
  `MembershipGeneration` exists, and never convert between unrelated ones
  implicitly.
* Validate every externally supplied value at the construction boundary and
  return a machine-readable `ErrorCode`, not a silently normalized value.
* Keep behavior deterministic. Any randomized test must fix its seed and print
  it, and the library must never read a wall clock on its own.
* Fix the concurrency model in writing before changing it. The documented
  model is: `RackRegistry` is internally synchronized and mutations are
  serialized end to end, including durable publication; the lock order is
  store writer mutex, then registry mutex, and it is never taken in reverse.
* Never emit callbacks while holding an internal lock; the library emits no
  callbacks while locked at all.
* Bound every externally influenced size before allocating, and bound every
  collection that can grow from untrusted input.
* The project compiles under MSVC `/W4 /WX` and under `-Wall -Wextra -Werror`
  on GCC/Clang. New code must be warning-free; fix the cause rather than
  suppressing the warning.
* There is no product TODO, placeholder handler, or dead code in this
  repository, and contributions must not add any.

## Testing expectations

Every behavioral change should come with proof:

* unit tests for the changed type or rule;
* property/invariant tests for anything with interval, ordering or count
  semantics;
* a persistence test through a real close/reopen when durable state changes;
* an adversarial test when new input is parsed.

Tests must not depend on the time of day or on fixed sleeps that are
load-bearing. Where a test needs a timestamp, it supplies one explicitly.

## Repository hygiene

Do not commit build directories, install trees, benchmark residue, crash
dumps, logs, or temporary fixtures. `git status` must be clean before a change
is proposed.

## Submitting a contribution

1. Open an issue or start a discussion describing the change if it is larger
   than a small fix.
2. Keep the change focused; unrelated reformatting makes review harder.
3. Make sure Release and Debug builds are warning-free and the full test suite
   passes.
4. Open a pull request with a neutral, descriptive commit message.

## License

By submitting a contribution to this repository you agree that your
contribution is licensed under the Apache License, Version 2.0, as described in
the `LICENSE` file in this distribution. There is no Contributor License
Agreement (CLA) and no copyright assignment requirement: contributions are
accepted under the inbound-equals-outbound terms of Section 5 of the license.

Do not add co-author trailers, AI attribution, or generated-by lines to
commits in this repository.
