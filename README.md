# Rack Registry

Rack Registry is the canonical rack-composition runtime of the Data Center
Control Plane (DCCP), Tranche 1: Canonical Facility State. It answers one
question:

> Which racks exist, what occupies them, which generation of that composition is
> authoritative, and which claims about it must be rejected as stale?

It is a vendor-neutral C++20 library with a small inspection tool, three
examples, a benchmark, and a test suite that includes real process-level crash
injection and multiprocess writer fencing. It has no third-party dependencies.

Version 1.0.0. Persisted state format version 1. Snapshot layout version 1.

---

## 1. Systems boundary

Rack Registry owns **racks as canonical facility objects** and the
**authoritative membership and occupancy composition inside each rack**. It
establishes what a rack is, how it is composed, which generation of that
composition is current, and whether a proposed change is allowed.

### What it owns

* Rack identity, generation, revision and lifecycle.
* The physical unit extent of a rack and the mounting coordinate model.
* Membership inside a rack: which member is at which coordinate, and which
  generation that placement belongs to.
* Occupancy, overlap and shared-mount rules, and structural free ranges.
* Power and cooling **domain associations** as typed references.
* Compatibility metadata: the profile a rack declares and the traits a member
  needs.
* Generation-bound mutation authority: every mutation states the generation it
  was planned against.
* Provenance for authoritative state, and bounded rejection explanations.
* Immutable snapshots, canonical deterministic serialization, generation
  diffing, a canonical state digest, transactional durable publication,
  writer-epoch fencing, and conservative recovery.

### What it explicitly does not own

| Adjacent runtime | What it owns, and this repository does not |
| --- | --- |
| Facility Topology | Facility graph, containment and topology generations |
| Physical Location Registry | General physical addressing and site coordinates |
| Asset Registry | Asset identity and asset-internal lifecycle |
| Facility Dependency Registry | Dependency edges between facility systems |
| Failure Domain Registry | Correlated-failure classification |
| Facility Capacity Reservation | Future capacity, reservations, allocation |
| Facility State Ledger | The authoritative history of the whole facility |
| Power and cooling control | Electrical and thermal actuation of any kind |
| Placement and scheduling | Deciding where a workload or asset should go |

External objects are referenced only through stable opaque identifiers
(`AssetId`, `PowerDomainReference`, `CoolingDomainReference`, compatibility
profile identities). Rack Registry never resolves them, never models their
internal semantics, and never contacts a remote service.

---

## 2. Architecture

```
include/rack_registry/   public API (19 headers, all installed)
src/                     implementation and the internal file/codec layer
tools/                   rackctl and the two process-level proof harnesses
examples/                three programs that use only the public API
tests/                   nine suites plus an independent downstream consumer
benchmarks/              completed-work benchmarks
```

The public surface is a library, not a framework:

* **`RackRegistry`** — the in-memory authoritative model. Internally
  synchronized, mutation-serialized, and free of callbacks.
* **`RackStore`** — the durable, transactional store. It owns writer authority
  for one state file and publishes generations atomically.
* **`RackSnapshot`** — an immutable, self-contained value. It is the
  serialization unit, the comparison unit, and the boundary between untrusted
  bytes and trusted state.
* **Value types** for identity, generations, coordinates, lifecycle,
  compatibility, provenance, requests and results.

Consumers never manipulate raw persistence structures. A mutation is a typed
request value; a rejection is a typed `RackError` with a stable numeric code.

---

## 3. Authority model

### Identities, generations and epochs

| Type | Meaning | Progression |
| --- | --- | --- |
| `RackId` | Canonical rack identity, text form `rack:...` | Immutable |
| `RackMemberId` | Identity of a membership record, `rm:...` | Immutable |
| `AssetId` | Opaque reference to an asset owned elsewhere | Immutable |
| `RackGeneration` | Generation of the whole rack record | +1 per accepted mutation, from 1 |
| `RackRevision` | Structural revision | +1 per accepted structural mutation, from 1 |
| `MembershipGeneration` | Generation of the membership | +1 per accepted membership mutation, from 0 |
| `StoreEpoch` | Writer authority epoch of a durable store | Advances on every acquisition |
| `StoreSequence` | Publication sequence of a durable store | +1 per published generation |

All of these are distinct types. There is no implicit conversion between them,
none of them converts to an integer implicitly, and there are no sentinel
values: absence is `std::optional`, failure is `Result<T>`.

### Preconditions are mandatory

There is no unconditional mutation in the public API.

* Structural and lifecycle mutations carry a `StructuralPrecondition` or a
  `LifecyclePrecondition`.
* Membership mutations carry a `MembershipPrecondition` with **both** the rack
  generation and the membership generation, because a structural change and a
  membership change are different ways for a plan to go stale.
* A lifecycle transition is a compare-and-swap: it names the state it expects.

A rejected mutation leaves the registry byte-for-byte unchanged, and the
rejection is recorded with its code, operation, subject, expected and actual
authority, and a human explanation.

### Idempotent retries

Each mutation may carry a `RequestId`. The rack keeps a bounded table of recent
request identities with a digest of the request's **intent** — the preconditions
are deliberately excluded, so a caller that retries the same intent with a
refreshed generation is recognised as a retry rather than as a conflicting
reuse. A recognised retry is answered from the table with `replayed = true` and
changes nothing; reusing an identity with genuinely different content is
rejected with `request_id_conflict`. The table is bounded per rack and the
number of evicted entries is preserved in the state, so bounded replay coverage
is visible rather than implied.

### Provenance

Every mutation carries a `ProvenanceRecord`: source, actor, optional source
reference and sequence, a caller-supplied observation time, and a note. The
library never reads a wall clock for authoritative state, so mutation outcomes
are reproducible and the recorded time is an explicit part of the claim. Rack
and member trails are bounded; evicted records are counted.

---

## 4. Mounting coordinates and occupancy rules

Vertical position is expressed in **mount slots**. One rack unit spans exactly
`kMountSlotsPerRackUnit = 2` slots, so half-unit mountings are exact:

```
rack unit U  <->  mount slots { 2U-1, 2U }
```

Every interval in this library is **half open**: `[begin, end)` contains
`begin` and excludes `end`. Two ranges are disjoint when one begins at or after
the other's end, so `[1,3)` and `[3,5)` never overlap. Unit ranges follow the
same rule but render inclusively (`U10-U12`) because that is how rack units are
spoken about; both parsers are strict and both round-trip.

Occupation rules, in full:

* A **`FullSpan`** member occupies its slots exclusively.
* A **`SharedSpan`** member may legally co-occupy its slots with other members
  that declare the **same shared-mount class over exactly the same interval**,
  up to a declared capacity. Co-occupants must agree on the capacity.
* A **`ZeroU`** member occupies no slot (side or rear rail) and never conflicts
  with anything, including another zero-U member. It is the only overlap that is
  always legal.
* Two non-zero spans that partially overlap always conflict, even if both are
  shared.
* Every member must lie inside the rack extent. Shrinking a rack so that an
  existing member would fall outside it is rejected
  (`rack_extent_would_evict_members`), and so is a profile change that would
  strand a member (`compatibility_unsatisfied`).

`free_spans()` reports unoccupied structural ranges and `shared_availability()`
reports shared coordinates with their remaining capacity. Both are
**descriptive**: they report what is physically unoccupied, not what may be
allocated. Capacity commitments belong to Facility Capacity Reservation. A
`Reserved` membership is a structural coordinate hold that occupies its span for
overlap purposes; it is not an allocation.

---

## 5. Lifecycle

```
Defined ──▶ Commissioned ──▶ Active ⇄ Maintenance
   │              │            │         │
   └──────────────┴────────────┴─────────┴──▶ Retired ──▶ Removed
```

`Maintenance` withdraws a rack from service without freezing it: members may
still be inserted, moved, replaced and removed, but the physical structure may
not be redefined. `Retired` and `Removed` are immutable: every mutation of any
kind is refused with `lifecycle_mutation_forbidden`. The capability table is:

| State | Membership | Structure | Domain associations |
| --- | --- | --- | --- |
| Defined | yes | yes | yes |
| Commissioned | yes | yes | yes |
| Active | yes | yes | yes |
| Maintenance | yes | no | yes |
| Retired | no | no | no |
| Removed | no | no | no |

A retired rack remains fully readable; it is authoritative history, not a gap.

---

## 6. Persistence, publication and recovery

### State file

One file holds one generation. A reader validates, in order:

1. the file size against the accepted bound, before reading anything;
2. the magic, format version, byte-order tag, coordinate model and payload kind;
3. the header digest;
4. the declared payload length against the bytes actually present;
5. the payload digest;
6. the trailer;
7. every declared length, count, enum domain and identity inside the payload,
   each against its documented bound and against the bytes that remain;
8. every registry invariant, including occupancy, which is re-validated before a
   decoded snapshot is returned.

A file that fails any step is rejected deterministically; nothing is normalized
into authoritative state. Two facts are pinned by tests because they are format
contract, not implementation detail:

* the encoding is **canonical** — equal state always produces identical bytes,
  and re-encoding a decoded image reproduces it byte for byte;
* the **state digest** covers identity, structure, counters, lifecycle and
  membership, and deliberately excludes history. Appending provenance or
  generation evidence therefore never changes the digest of the state it
  describes, which is what makes the retained generation chain verifiable: the
  newest evidence entry must equal the current state digest.

### Publication

```
plan ─▶ validate ─▶ write temporary ─▶ flush ─▶ read back and verify
     ─▶ retain previous ─▶ re-check writer authority ─▶ atomic replace
     ─▶ retire temporary
```

The atomic replace is the authoritative completion boundary. Before it, a
failure retires the temporary artefacts and rolls the in-memory image back to
the generation that is still authoritative. After it, the new generation is
durable and nothing reported afterwards may undo it. A crash at any point leaves
either the previous generation or the new one authoritative, never a mixture: a
temporary file is never read as state, and recovery retires it on the next open.

### Writer authority and fencing

One writer per state file. Authority is a lock record containing the writer
identity, a fresh incarnation, the process identifier, the process start marker,
the store epoch and the acquisition time. Acquisition is race-free: the record
is written to a temporary name and published with an atomic create-or-fail, then
read back to confirm that this incarnation won. Exactly one contender can
observe its own incarnation.

* A **live holder** excludes every other writer (`writer_lock_held`).
* A holder that is **provably gone**, or a lock record that fails its own
  integrity check, is adopted with a strictly higher epoch.
* `force_takeover` is the only path that takes authority away from a live
  writer. It is always an explicit operator action, it records who was fenced
  and at which epoch, and it names no live process so the next writer adopts it
  normally.
* Authority is **re-validated immediately before the atomic replace**. A writer
  whose epoch was taken over while it was planning is refused with
  `stale_writer_fenced` and publishes nothing.
* Release rewrites the record with no process behind it instead of deleting it,
  so epochs stay strictly increasing across clean restarts and the last holder
  stays visible. `query_writer_lock` reports that state as `unlocked`.

### Persisted evidence does not become fresh on restart

Generations, revisions, epochs, publication sequences, provenance trails,
generation evidence and the idempotency table all survive restart. A restarted
process resumes at the generation the store records and must satisfy exactly the
same preconditions as any other consumer. There is no warm-up path that resets a
counter or re-admits stale authority.

### Recovery

| Situation | Action |
| --- | --- |
| No state file | Empty store, epoch 1, sequence 0 |
| Current file verifies | `loaded_current`; the generation is authoritative |
| Current file missing/corrupt, retained file verifies | `loaded_previous`; the corrupt file is left untouched for the operator |
| No generation verifies | Open fails with the rejection that explains why; no empty store is substituted |

A corrupt current generation is never rewritten behind the operator's back, and
a store that cannot establish authority refuses to start rather than starting
empty.

---

## 7. Concurrency model

* Every public method of `RackRegistry` and `RackStore` is safe to call from any
  thread.
* Mutations are serialized. On a store, one mutation holds the store mutex from
  planning through the durable flush, so a query never observes a mutation that
  has been applied in memory but not yet published.
* Queries return self-contained values. A reader observes the state before a
  mutation or the state after it, never a partial application, and can never
  hold a reference into mutable state.
* Lock order is **store mutex, then registry mutex**. The registry never calls
  into the store, so the order cannot invert.
* The library emits **no callbacks**. It cannot re-enter itself, cannot call a
  consumer while holding a lock, and has no subscription or event surface.
* There is no asynchronous work, no worker pool and no queue, so there is
  nothing to cancel and no stale completion that could publish after
  cancellation. Cancellation is therefore not part of the API; shutdown is
  `close()`, which takes the store mutex (waiting for any in-flight mutation to
  finish), retires every temporary file the store still owns, releases writer
  authority, and returns accounting to its baseline. It is idempotent, and a
  store that is destroyed without `close()` performs the same work.
* Bounds are enforced before allocation everywhere: rack count, member count per
  rack, rack units, domain associations, traits, provenance trails, retained
  generations, idempotency records, rejection journal, every text length, and
  the accepted state-file size.

The only seam that runs code while a lock is held is the failure-injection hook
(`StoreOptions::fault_hook`). It is empty in every normal configuration — the
store then performs no indirect call along the publication path at all — and a
hook must not call back into the store or a registry. Its supported behaviours
are to throw and to terminate the process, which is how the test suite proves
rollback and crash recovery respectively.

### Lock and reentrancy audit

The concurrency model was audited by inspecting call paths, not only by testing:

* no read-then-write upgrade anywhere: mutations take the exclusive lock
  directly and queries take the shared lock;
* no lock is held across a call that can re-enter the same state: the registry
  never calls the store, and the store calls the registry only while holding its
  own mutex, in the documented order;
* no callback is invoked while a lock is held, because there are no callbacks;
* no worker is joined while holding state it needs, because there are no
  workers;
* shutdown takes the same mutex as a mutation, so it cannot deadlock against
  one, and it leaves no temporary file or lock record behind.

Known residual window, stated rather than hidden: releasing writer authority is
a verify-then-replace. If an operator forces a takeover in the microsecond
between another process verifying that it still owns the lock and replacing it,
the newer lock record can be overwritten. The consequence is bounded and fails
closed — the affected writer's next publication re-checks authority, finds a
lock that is not its own, and is refused with `stale_writer_fenced`. No
generation can be torn, and no two writers can publish different generations
concurrently. Closing that window completely would require an operating-system
advisory lock, which would also remove the ability to adopt a dead writer's
authority or to fence a live one; that trade was made deliberately.

---

## 8. Errors

`ErrorCode` is a stable numeric contract, grouped by fault domain and never
reused:

| Range | Category | Examples |
| --- | --- | --- |
| 100–199 | Input | `malformed_identity`, `invalid_character`, `invalid_utf8` |
| 200–299 | Identity | `duplicate_rack_id`, `duplicate_asset_placement` |
| 300–399 | Authority | `stale_rack_generation`, `stale_writer_fenced`, `generation_not_retained` |
| 400–499 | Lifecycle | `lifecycle_transition_not_allowed`, `lifecycle_mutation_forbidden` |
| 500–599 | Occupancy | `occupancy_overlap`, `mount_out_of_bounds`, `shared_mount_capacity_exceeded` |
| 600–699 | Compatibility | `compatibility_unsatisfied` |
| 700–799 | Persistence | `integrity_check_failed`, `unsupported_format_version`, `truncated_state` |
| 800–899 | Limits | `limit_exceeded`, `snapshot_invalid` |
| 900–999 | Writer | `writer_lock_held`, `invalid_writer_lock`, `read_only_store` |

`code_name()` gives a stable identifier, `explain()` gives one sentence
independent of any occurrence, `describe()` renders a rejection with its
structured context, and a `RackError` carries the operation, subject, related
identity, expected and actual counters, and the ordered list relevant to the
decision (for example the missing traits). Rejections of mutations are also
retained in a bounded journal for inspection.

---

## 9. Building, installing and consuming

### Build and test

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `RR_BUILD_TESTS`, `RR_BUILD_TOOLS`, `RR_BUILD_EXAMPLES`,
`RR_BUILD_BENCHMARKS`, `RR_WARNINGS_AS_ERRORS` (default ON),
`RR_SANITIZERS`, `BUILD_SHARED_LIBS`.

The suite contains **no timeouts and no watchdog**. A test that does not
terminate is treated as a defect to diagnose, never as a pass.

### Install

```
cmake --install build --prefix <prefix>
```

This installs the library, the 19 public headers, `rackctl`, and the CMake
package (`RackRegistryConfig.cmake`, `RackRegistryConfigVersion.cmake`,
`RackRegistryTargets.cmake`) under `<prefix>/lib/cmake/RackRegistry`, plus
`README.md`, `LICENSE` and `NOTICE` under the documentation directory.

### Consume

```cmake
find_package(RackRegistry 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE RackRegistry::rack_registry)
```

An independent consumer that lives outside this source tree and uses only the
installed package is included at `tests/downstream`:

```
cmake -S tests/downstream -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=<prefix>
cmake --build <build>
ctest --test-dir <build> --output-on-failure
```

### Minimal use

```cpp
#include <rack_registry/rack_registry.hpp>
using namespace rackregistry;

RackRegistry registry;

RegisterRackRequest registration;
registration.structure.id = RackId::parse("rack:a01").value();
registration.structure.unit_count = 42;
registration.structure.profile.id = CompatibilityProfileId::parse("cp:general").value();
registration.structure.profile.provides = TraitSet::parse("power.ac.208v").value();
registration.identity.provenance.actor = ActorId::parse("deploy").value();
const auto registered = registry.register_rack(registration);

InsertMemberRequest insert;
insert.rack_id = registration.structure.id;
insert.precondition.expected_generation = registered.value().generation;
insert.precondition.expected_membership_generation = registered.value().membership_generation;
insert.member_id = RackMemberId::parse("rm:server-01").value();
insert.asset_id = AssetId::parse("asset:server-01").value();
insert.mount = MountSpan::full_units(RackUnitRange::inclusive(1, 2).value()).value();
insert.requirements.required = TraitSet::parse("power.ac.208v").value();
insert.identity.provenance = registration.identity.provenance;
const auto placed = registry.insert_member(insert);
```

Note the shape of the API: `Result` has no implicit conversion to `bool`, and
the rvalue `value()` returns **by value** rather than by reference, so
`registry.occupancy(id).value()` can be iterated safely while a stored `Result`
is read through the reference overloads.

---

## 10. Inspection tool

`rackctl` exercises the same library a consumer uses. It never bypasses a
generation check or a lifecycle gate, and every mutating command requires the
caller to state the generation it expects.

```
rackctl register rack:a01 --units 42 --profile cp:general --provides power.ac.208v
rackctl insert rack:a01 --member rm:srv-1 --asset asset:srv-1 --mount U1-U2 \
        --expected-generation 1 --expected-membership-generation 0
rackctl occupancy rack:a01
rackctl free rack:a01
rackctl show rack:a01
rackctl diff rack:a01
rackctl verify
rackctl lock-status
rackctl takeover --writer wr:operator
```

Read-only commands (`list`, `show`, `members`, `occupancy`, `free`, `shared`,
`diff`, `verify`, `recover`, `lock-status`, `rejections`, `export`) open the
store **without taking writer authority**, so they run while another process
owns it. `--json` gives machine-readable output for `list`, `show`, `verify`
and `lock-status`. Exit codes are 0 success, 1 usage error and 2 rejected
operation, and every rejection is printed as `code_name(code_value): message`
with its structured context.

`diff` with no `--from` compares the authoritative generation against the
retained previous publication and therefore works across restarts. `--from N`
compares generations retained in the running process; when the requested
generation is exactly the retained previous publication, the tool reports that
comparison instead of an error.

---

## 11. Validation performed

All evidence below was produced on Windows x64 with MSVC 19.44.35222, CMake
4.3.2 and Ninja 1.13.2. Nothing in this repository claims accelerator, network,
power, cooling, PDU, UPS or switch hardware integration; none was exercised and
none is simulated. Benchmarks are synthetic and labelled as such.

### Builds and suites

| Configuration | Result |
| --- | --- |
| Release, `/W4 /WX /permissive-` | Clean build, 9/9 suites pass |
| Debug, `/W4 /WX /permissive-` | Clean build, 9/9 suites pass |
| RelWithDebInfo with AddressSanitizer (`/fsanitize=address`) | Clean build, 9/9 suites pass |

Zero first-party warnings in every configuration; warnings are errors, so a
warning cannot be ignored. UndefinedBehaviorSanitizer is not available on MSVC,
so it was not run and is not claimed.

### Suites

| Suite | What it proves |
| --- | --- |
| `test_text_mount` | UTF-8 acceptance and rejection, identity grammars, half-open interval algebra, coordinate boundaries, mount-span conflict rules, SHA-256 against published vectors |
| `test_registry_model` | Registration, structure replacement, the lifecycle table, membership mutations, occupancy invariants, shared mounts, idempotency, deterministic ordering, generation diffing, snapshot/restore |
| `test_serialization` | Byte-level format contract, canonical round-trip, every header check rejected precisely, payload bit-flip detection, a hand-assembled image that violates an occupancy invariant |
| `test_property` | Randomized sequences against an independent bitmap and placement model, exhaustive boundary placement, half-unit adjacency, generation replay through bytes, stale-precondition sweeps, order independence — every seed fixed and printed |
| `test_adversarial` | 4,000 mutation/truncation/extension/reordering cases against the reader, integer-overflow length declarations, malformed identities, documented bounds at and one past the limit, missing directories, empty and oversized files |
| `test_persistence` | Fresh store, publication, reopen, read-only observers, second-writer refusal, rollback at every pre-commit fault point, post-commit faults that must not undo published work, previous-generation fallback, a damaged current generation never being promoted into the retained slot, stale temporary retirement, adoption, operator fencing, retained-previous diffing |
| `test_concurrency` | Concurrent readers against a writer, precondition-serialized mutators with exact accounting, concurrent store queries, shutdown with work in flight |
| `test_crash_recovery` | A real child process aborted at each named durable step; the parent reopens and proves the previous generation is byte-identical, and that a crash during a multi-record occupancy update applies all or nothing |
| `test_multiprocess` | A live holder excluding a second process, authority released when a holder is killed, operator fencing stopping a live writer from publishing, two processes racing with exactly one winner, sequential writers accumulating state |

The randomized suites print their seeds. The crash and fencing suites start real
operating-system processes through `rr_crash_child` and `rr_fence_child`; those
harnesses are built only when the test suite is enabled and are not installed.

### Benchmarks

Measured on the machine above, Release, `--racks 64 --members 32`. Each value
times a **completed** operation at that scale, and the durable figures include
the whole publication sequence: serialization, temporary write, device flush,
read-back verification, retention of the previous generation and the atomic
replace. They are not production-facility figures and no precision beyond the
printed digits is implied.

| Benchmark | Scale | Completed operations | Per operation | Throughput |
| --- | --- | --- | --- | --- |
| Occupancy validation | 64 racks × 32 members | 256 rejected inserts frozen against the full membership | 6.7 µs | 149,332/s |
| Membership mutation | 64 racks × 32 accepted inserts | 2,048 | 78.2 µs | 12,790/s |
| Deterministic enumeration | 64 racks × 32 members × 20 repeats | 1,280 | 16.2 µs | 61,667/s |
| Generation diff | 64 racks × 32 members | 64 | 28.8 µs | 34,737/s |
| Durable registration | 64 racks | 64 | 12.3 ms | 81/s |
| Durable membership mutation | 64 racks × 4 inserts | 256 | 11.6 ms | 86/s |

The durable figures are dominated by the two device flushes and the two full
file copies per publication (current and retained previous) at a ~69 KB state.
That is the cost of the durability guarantee, measured rather than estimated.

### Packaging

* Install into a clean prefix produces the library, the 19 headers, `rackctl`,
  the package configuration files and the documentation.
* `tests/downstream` configures with `find_package(RackRegistry 1.0 REQUIRED)`
  against that prefix from outside the source tree, builds with `/W4 /WX`,
  runs, and exercises registration, membership, idempotent retry, occupancy
  queries, persistence, reopen and inspection.
* The examples and the inspection tool were run against real state files.

---

## 12. Genuine limitations

* **One writer per state file.** Concurrency across processes is deliberately
  exclusive; there is no consensus, no replication and no multi-writer merge.
* **The retained previous publication is the restart-time diff boundary.** The
  in-process ring of previous generations starts empty when a store is opened,
  so arbitrary historical diffs are available only for the last publication
  boundary or for generations produced in the running process.
* **Generation evidence is a digest chain, not a full history file.** It proves
  that the current generation is consistent with what was recorded, and it does
  not reconstruct an arbitrary past generation.
* **History is bounded.** Provenance trails, generation evidence, idempotency
  receipts and the rejection journal all drop their oldest entries once their
  bound is reached, and each records how many were dropped.
* **The state file is one generation, not a log.** Publication is a full
  rewrite; the durability cost is therefore proportional to state size, as the
  benchmarks show.
* **Residual writer-release window.** Documented in section 7: an operator
  force-takeover that races another process's release can overwrite the newer
  lock record. The failure mode is fail-closed, not corruption.
* **No sanitizer other than AddressSanitizer was run.** MSVC does not provide
  UndefinedBehaviorSanitizer, so undefined-behaviour checking on this platform
  rests on the compiler, the invariant tests and the adversarial suite.
* **Validation is Windows/MSVC only.** The code has a POSIX path for every
  platform operation, but that path was not built or executed during this
  validation and is therefore not claimed as proven.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
