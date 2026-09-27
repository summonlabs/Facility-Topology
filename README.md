# Facility Topology

Generation-bound structural model of a data center facility: buildings, halls,
rooms, rows, racks, zones, containment, physical adjacency, references to power
and cooling domains, and facility-level dependency structure.

**DCCP position.** Data Center Control Plane (DCCP), Tranche 1 — Canonical
Facility State. Repository 2 of 8. DCCP is the facility-wide composition and
authority layer above Accelerated Systems Infrastructure (ASI) and Distributed
Fabric Infrastructure (DFI). This repository owns the authoritative physical
model of the data center: what exists, where it exists, how it is related, which
generation is current, and which state higher control layers may trust.

* Portable C++20 library, CMake, no third-party dependencies.
* Immutable, generation-bound snapshots; publication is atomic and durable.
* Deterministic canonical serialization with an integrity digest.
* Installable and exported as `dccp::facility_topology`.

---

## 1. Systems boundary

### What this repository owns

* The generation-bound **structural graph** of one or more facilities:
  containment (buildings, halls, rooms, rows, racks, zones), physical adjacency,
  and typed references to power and cooling domains.
* **Node and edge identity**, node kinds, boundary kinds, adjacency kinds and
  domain kinds, with a declarative structural schema.
* **Generation transitions**: base generation plus a validated mutation set
  produces a candidate generation; only a validated candidate is published.
* **Immutable snapshots** with canonical ordering, deterministic traversals,
  deterministic diffs and deterministic *rejections*.
* **Provenance**: which actor, from which source, for what reason, introduced
  each record, and under which generation and writer authority it was published.
* **Durable, integrity-checked persistence** of published generations, with
  conservative recovery after a crash or partial write.
* **Deterministic export** of one generation, suitable for later capacity,
  placement, failure-domain and observability consumers.

### What this repository explicitly does not own

| Area | Owner |
| --- | --- |
| Detailed asset inventory (models, serials, specifications) | Asset Registry |
| Rack membership, occupancy and allocation policy | Rack Registry |
| Stable coordinate, address and naming semantics | Physical Location Registry |
| General cross-service dependency semantics | Facility Dependency Registry |
| Network topology, paths, transport, congestion, fabric federation | Distributed Fabric Infrastructure (DFI) |
| Accelerator execution, memory, serving, scheduling | Accelerated Systems Infrastructure (ASI) |
| Capacity planning, placement optimisation, reservations | future consumers |
| Electrical actuation, cooling control, BMS integration | power/cooling control planes |
| Maintenance orchestration, tenancy policy, incident response, dashboards | other control planes |
| Multi-site federation | out of scope for Tranche 1 |

Power and cooling domains are **references only**. A `DomainAssociation` records
that a structural node references a domain declared in the same generation. It
carries no capacity, feed topology, actuation, propagation or control semantics,
and this repository never actuates anything.

A **zone** is a structural boundary region declared inside a facility, grouping
nodes of exactly one declared kind. It is not a network segment, not a failure
domain, and not a tenancy boundary; those are separate concerns.

### External references

ASI and DFI objects may be referenced only through stable opaque identifiers.
This repository does not model accelerator resources, network paths, link state
or transport, and contains no hardware integration of any kind.

---

## 2. Architecture

```
include/dccp/facility_topology/     public API (installed)
  result.hpp        ErrorCode, ErrorCategory, Error, Result<T>, FT_TRY
  strong_id.hpp     NodeId, DomainId, ActorId, MutationId, TopologyGeneration, WriterEpoch
  text.hpp          strict UTF-8, canonical decimal, hex, quoting, identifier grammar
  digest.hpp        SHA-256 (FIPS 180-4) integrity digest
  clock.hpp         injected time source; canonical UTC formatting and parsing
  model.hpp         NodeKind, BoundaryKind, AdjacencyKind, DomainKind, records, limits
  topology.hpp      TopologySnapshot, TopologyBuilder, PublishedTopology, validation report
  mutation.hpp      Mutation commands, MutationBatch, MutationOutcome, dispositions
  canonical.hpp     canonical document and generation manifest, batch identity
  diff.hpp          deterministic structural difference between generations
  store.hpp         TopologyStore, open modes, epochs, recovery, export/import
src/                                          library implementation (not installed)
tools/ftopctl/                                inspection and operations CLI
examples/                                     compiled and run during validation
tests/                                        proof obligations
tests/downstream/                             independent find_package consumer
benchmarks/                                   completed-operation benchmarks
```

**Storage and views are separate.** `TopologyBuilder` is the only mutation
surface for structure; it accepts untrusted records one at a time, enforces every
structural precondition immediately, and produces an immutable
`TopologySnapshot` only after whole-graph validation. Consumers read snapshots
and never touch persistence structures: `GraphData`, the internal storage type,
is forward-declared in the public headers and never defined there.

`TopologySnapshot` is a value type over shared immutable storage. Copying it is
cheap and every copy observes exactly the same structure. A published generation
can never change underneath a reader.

### Structural schema

Containment is a forest of **physical** boundary trees, one tree per facility
root, plus **logical** boundary edges that express zone membership.

| Kind | Canonical depth | May be physically contained by | May physically contain | Zonable |
| --- | --- | --- | --- | --- |
| `facility` | 0 | — (root only) | building, hall, room, row, rack, zone | no |
| `building` | 1 | facility | hall, room, row, rack | no |
| `hall` | 2 | facility, building | room, row, rack | yes |
| `room` | 3 | facility, building, hall | row, rack | yes |
| `row` | 4 | facility, building, hall, room | rack | yes |
| `rack` | 5 | facility, building, hall, room, row | — | yes |
| `zone` | 1 | facility | — (members arrive by logical edges) | no |

Additional rules, all enforced incrementally and re-checked by whole-graph
validation:

* exactly one physical parent per node; a facility has none and every other node
  has exactly one;
* a zone groups nodes of exactly one declared kind (`zone_member_kind`), and a
  node belongs to at most one zone;
* a zone may only group nodes of its own facility;
* adjacency is symmetric, stored with the lesser identity first, defined only
  between two physical non-facility nodes of the same facility, and at most one
  adjacency edge exists per pair;
* a domain association references a domain declared in the same generation and
  its kind must match the declaration;
* no containment cycles, physical or logical;
* maximum structural depth 5.

---

## 3. Identity, generation and authority semantics

Every identity and counter is a distinct type. There are no implicit conversions
between node, domain, actor, mutation, store, generation and epoch values, and no
sentinel values: absence is `std::optional` or an explicit sum type.

| Type | Meaning |
| --- | --- |
| `NodeId`, `DomainId`, `ActorId`, `MutationId` | validated identifiers (1..128 bytes, ASCII alphanumeric first/last, interior `[0-9A-Za-z._:-]`) |
| `TopologyGeneration` | monotonic published generation; 0 means "nothing published yet"; overflow is reported, never wrapped |
| `WriterEpoch` | durable mutation authority of a writer incarnation; 0 means "no durable authority claimed" |

**Generation transition.**
`base generation + validated mutation set -> candidate generation -> integrity
validation -> atomic publish`. A mutation batch is the unit of atomicity: either
every mutation in it is applied and a new generation is published, or none is and
the previous generation remains authoritative.

**Published generations are immutable.** A generation file is written once,
verified, and atomically renamed into place. It is never rewritten, and a later
generation never mutates an earlier one.

**Provenance** is attached to every node, containment edge, adjacency edge, domain
declaration and domain association: actor, source, optional reason, optional
canonical UTC timestamp. The generation manifest additionally records the parent
generation and its document digest, the publishing actor, the writer epoch, the
batch identity, the retention floor and the recent-batch window.

**Mutation authority.** Opening a store for mutation reserves a fresh
`WriterEpoch` under the store lock and then releases the lock. The epoch, not a
held lock, is the authority: every commit is checked against the epoch file under
the lock, so a writer whose authority was superseded — by another process, in
this process, or after a restart — is refused with `STALE_AUTHORITY_EPOCH`. A
batch whose base generation is no longer current is refused with
`STALE_BASE_GENERATION`.

**Idempotency.** Every durable commit and creation carries a `MutationId`. The
identity is remembered, with the digest of the request content, in a bounded
window carried forward by each generation. Replaying an identity with identical
content publishes nothing and reports the generation the original application
produced; reusing an identity for different content is refused with
`IDENTITY_CONFLICT`. The writer epoch is deliberately *not* part of the content
digest, so a retry after reopening a store is still recognised as the same
request.

---

## 4. Persistence, integrity and recovery

A store is a directory:

```
LOCK                              advisory lock file
EPOCH                             highest issued writer epoch
HEAD                              published head marker: generation + document digest
generations/gen-<20 digits>.ftop  committed generations, immutable
pending/                          temporary files, never authoritative
quarantine/                       files rejected by integrity checks, preserved
```

**Commit protocol.** plan -> validate -> reserve transaction identity -> write
and flush a temporary generation -> verify integrity -> **atomically rename it
into `generations/`** -> replace the head marker -> retire superseded temporary
state. The atomic rename is the commit point; `HEAD` is a derived marker that
recovery can always rebuild from the committed generations.

**Integrity.** Every generation document ends with `digest sha256:<hex>` over
every preceding byte after the banner. The digest detects corruption; structural
validation decides whether authoritative state may be reconstructed. A document
whose digest matches but whose content is impossible is still rejected.

**Recovery is conservative.** `recover()`:

1. discards temporary files that never reached their commit point — they carry no
   authority and can never change what the store reports;
2. verifies every generation file and **quarantines, never rewrites**, anything
   that fails schema or integrity checks, including a head marker that is not a
   regular file;
3. restores the retention bound if a crash happened before pruning;
4. re-points `HEAD` at the highest generation that verifies — **at its own
   generation number**. Recovery never invents a generation, never renumbers
   evidence, and never promotes unpublished temporary state to authoritative
   state.

A store with no verified generation is left uninitialised rather than guessed at,
and creating a new generation over quarantined evidence is refused so that the
evidence cannot be silently buried.

**Bounded growth.** Each store handle has explicit limits (`TopologyLimits`):
node, edge, adjacency, domain, association, batch, label, reason, document, line,
traversal and retention bounds. Retention keeps the newest `retained_generations`
generation files; recovery restores that bound if a crash interrupted pruning.

---

## 5. Deterministic rejection and error semantics

Failure is a value, not an exception: every fallible entry point returns
`Result<T>`, and `Error` carries a stable machine-readable `ErrorCode`, a
category, a human explanation and an optional subject. Codes are appended to,
never renumbered or repurposed, and `error_code_name()` is part of the contract.
Nothing in the library throws for an expected failure mode; a `Result` is
dereferenced only after it is known to hold a value.

**Deterministic rejection.** A batch is rejected as a whole and reports one
`MutationDisposition` per mutation it reached, each with the mutation's index,
its own stable code, an explanation and a subject. A failure of whole-graph
validation, which no single mutation owns, is reported with the index one past
the last mutation. `MutationOutcome::first_failure` carries the specific code of
the first failure and is the code a caller sees when a rejection is reported as
an error.

**Canonical ordering is enforced, not repaired.** Records must appear in
canonically increasing order — nodes by identity, containment by (parent, child),
adjacency by (first, second), domains by identity, associations by (node, domain)
— and parse/serialize round trips are byte-exact. A document whose records are
out of order, or that spells a field with the wrong quoting, is rejected rather
than normalised. The same applies to adjacency endpoints, which must be written
lesser identity first.

Untrusted input is rejected on: unsupported banner, unknown keyword, wrong field
count, bad enum token, malformed identity, non-canonical order, duplicate record,
count mismatch, truncated input, digest mismatch, oversized document or line,
unparseable number, invalid UTF-8, control characters, and structurally invalid
graphs.

---

## 6. API

### Build, validate and query

```cpp
#include <dccp/facility_topology/topology.hpp>
using namespace dccp::facility_topology;

auto generation = assemble_topology(nodes, containment, adjacency, domains, associations,
                                    TopologyGeneration(1));
if (!generation) { /* generation.error().to_string() */ }

TopologyStats stats = generation->stats();
std::vector<NodeId> ancestors = generation->ancestry(rack);
std::vector<NodeId> neighbours = generation->neighbors(rack);
std::vector<NodeId> members = generation->nodes_in_domain(DomainId::parse("pwr-a").value());

TraversalLimits bounds;                         // complete or it fails
bounds.max_depth = 32;
bounds.max_nodes = 1'000'000;
auto visited = generation->traverse(hall, TraversalOrder::DepthFirstPreOrder, bounds);
```

`TopologyBuilder` is the mutation surface: `add_node`, `remove_node`,
`set_node_label`, `add_containment`, `remove_containment`, `move_node`,
`add_adjacency`, `remove_adjacency`, `declare_domain`, `retire_domain`,
`add_association`, `remove_association`, then `validate()` and
`build(generation)`. Each mutation returns `Result<void>` with a stable code.

A traversal is **complete or it fails**: if the tree below the root is deeper
than `max_depth`, or larger than `max_nodes`, the call fails with
`TRAVERSAL_DEPTH_EXCEEDED` or `LIMIT_EXCEEDED` instead of returning a silently
truncated result.

### Apply mutations

```cpp
MutationBatch batch;
batch.base_generation = current->generation();
batch.actor = ActorId::parse("ops").value();
batch.source = "cli";
batch.reason = "add a rack";
batch.mutation_id = MutationId::parse("batch-0001").value();
batch.mutations = {AddNode{rack}, AddContainment{edge}};

BatchApplication application = apply_batch_explained(*current, batch, TopologyLimits{});
std::cout << application.outcome.explain();
```

### Durable store

```cpp
auto store = TopologyStore::open("/var/lib/dccp/facility-1", OpenMode::ReadWrite);
if (!store) { /* STORE_LOCKED, STORE_NOT_FOUND, PATH_INVALID, ... */ }

CreateOptions create;
create.actor = ActorId::parse("ops").value();
create.source = "cli";
create.mutation_id = MutationId::parse("create-1").value();
store->create(initial_snapshot, create);            // generation 1

batch.authority_epoch = store->epoch();
store->commit(batch);                               // atomic, durable
store->head();                                      // immutable snapshot
store->load_generation(TopologyGeneration(1));      // retained history
store->export_head_to("exported.ftop");
store->recover();
```

`CommitOptions::stop` carries a `std::stop_token`. Cancellation is observed only
before the commit point: a cancelled commit publishes nothing, and a commit that
has passed the commit point always completes.

### In-process publication

`PublishedTopology` is the one place where many threads read one generation:
readers call `current()` and receive a shared handle to an immutable snapshot;
one publisher at a time may `publish()`, and a second concurrent publisher is
refused immediately rather than blocked.

---

## 7. Concurrency model

* **Snapshots are immutable.** Any number of threads may read one snapshot
  concurrently without synchronisation.
* **A store handle belongs to one thread.** `TopologyStore` is movable, not
  copyable, and not thread-safe. Use one handle per thread.
* **Cross-process safety comes from the durable model, not from locks held
  across a session.** Opening for mutation takes the exclusive store lock only
  to reserve a writer epoch, then releases it. Commits and reads take the lock
  for the duration of one operation and release it before returning.
* **No callback is ever invoked while internal state is being changed**, and the
  library takes no lock that a caller-supplied function could re-enter: there are
  no caller-supplied callbacks in the library at all.
* **Lock acquisition never blocks.** A lock held by another process is reported
  as `STORE_LOCKED`, so there is no lock ordering, no deadlock, and no waiting
  inside the library.
* **No asynchronous work.** Publication is synchronous; there are no background
  workers, queues, subscriptions or completions that could publish stale results
  after a cancellation or an authority change.
* **Shutdown is `close()`**: it releases any authority the handle holds, is
  idempotent, and leaves the store in a state a later handle can open.

---

## 8. Inspection tool

`ftopctl` performs safe read-only inspection and narrowly scoped mutation. It
never bypasses the authority model: `apply` and `import` go through
`TopologyStore::commit` and `create`, so generation preconditions, writer epochs,
idempotency and atomic publication behave exactly as they do for any other
consumer. All commands print one `key=value` record per line, and rejection
prints `error-code=`, `error-category=`, `error-message=` and, when present,
`error-subject=`. Exit codes: `0` success, `1` rejected, `2` usage error.

```
ftopctl version
ftopctl status <dir>
ftopctl generations <dir>
ftopctl show <dir> [--generation N]
ftopctl nodes <dir> [--kind K] [--generation N]
ftopctl node <dir> --id ID [--generation N]
ftopctl ancestry <dir> --id ID [--generation N]
ftopctl descendants <dir> --id ID [--order bfs|dfs]
ftopctl neighbors <dir> --id ID [--generation N]
ftopctl domain <dir> --id DOMAIN [--generation N]
ftopctl diff <dir> --from N --to M
ftopctl validate <file>
ftopctl demo <dir> [--racks N]
ftopctl import <dir> --file F --actor A --mutation-id ID [--source S] [--reason R]
ftopctl apply <dir> --actor A --mutation-id ID [--base N] [--script F]
ftopctl export <dir> --out FILE
ftopctl recover <dir>
```

A mutation script is one command per line; `#` starts a comment.

```
add-node <id> <kind> ["label"] [zone-scope]
remove-node <id>
move-node <id> <new-parent>
add-containment <parent> <child> [physical|logical]
remove-containment <parent> <child>
set-label <id> "label"
add-adjacency <first> <second> [shared-boundary|service-aisle|structural-neighbor]
remove-adjacency <first> <second> [kind]
declare-domain <id> <power|cooling> ["label"]
retire-domain <id>
add-association <node> <domain> <power|cooling>
remove-association <node> <domain>
```

The mutation identity is tied to the request content, which includes the base
generation. A retry must therefore name the same base as the original attempt
(`--base N`); repeating an identical request is reported as `replayed=yes`, while
reusing the identity for different content is reported as
`error-code=IDENTITY_CONFLICT`.

---

## 9. Canonical serialization

A generation is stored and exported as line-oriented canonical text. The first
line is the banner `ftop/1`; the last is `digest sha256:<hex>` over every
preceding byte after the banner.

```
ftop/1
generation 2
parent 1
parent-digest sha256:<hex>
authority-epoch 3
actor ops
source "cli"
reason "add a rack"
recorded-at 2026-02-01T00:00:00Z
mutation-id batch-0001
retention-floor 1
applied 1
replay batch-0001 sha256:<hex> 2
nodes 12
node rack-03 rack "Rack 03" - ops "cli" "add a rack" 2026-02-01T00:00:00Z
...
containment 13
contain <parent> <child> <physical|logical> <actor> "source" <"reason"|-> <timestamp|->
adjacency 3
adjacent <first> <second> <kind> <actor> "source" <"reason"|-> <timestamp|->
domains 2
domain <id> <power|cooling> <"label"|-> <actor> "source" <"reason"|-> <timestamp|->
associations 3
associate <node> <domain> <power|cooling> <actor> "source" <"reason"|-> <timestamp|->
digest sha256:<hex>
```

Identities, enum tokens, numbers and timestamps are bare fields; human text is
quoted with `\"` and `\\` escapes; `-` means absent. Records appear in canonical
order and the parser enforces it. Two digests are distinguished:

* **content digest** — structural content only (nodes, containment, adjacency,
  domain declarations, associations), excluding generation, lineage, provenance
  and timestamps, so two generations describing the same facility share it;
* **document digest** — every byte of the canonical document, including lineage.

---

## 10. Build, test, install and consume

Requirements: CMake 3.20+, a C++20 compiler (validated with MSVC 19.44), and no
third-party dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/dccp
```

Options: `FACILITY_TOPOLOGY_BUILD_TESTS`, `..._BUILD_TOOLS`,
`..._BUILD_EXAMPLES`, `..._BUILD_BENCHMARKS`, `..._WARNINGS_AS_ERRORS` (default
`ON`), `..._ENABLE_ASAN` (default `OFF`).

Install produces headers, the static library, `ftopctl`, the CMake package
configuration and version files, and the documentation. Consumers use:

```cmake
find_package(facility_topology 1.0 REQUIRED)
target_link_libraries(app PRIVATE dccp::facility_topology)
```

`tests/downstream/` is an independent CMake project that consumes the installed
package from outside the source tree, exercises the public API, and runs.

---

## 11. Validation performed

All figures below were produced on the development host described in section 12.
The test suite is 133 tests; every test runs to completion, and the suite is
deterministic given its seed.

| Area | Evidence |
| --- | --- |
| Unit and integration | 133 tests across text, digest, model, topology, mutation, canonical, store, recovery, property, adversarial, concurrency, white-box validator, and independent-process suites |
| Property tests | seeded generators for valid and invalid graphs, mutation batches, moves, removals and generation races; seed printed with every failure; sweep over seeds 1, 7, 424242, 999983, 123456789 — all green |
| Cycle and orphan prevention | every attempt to reparent a container under its own descendant is rejected; dropping a physical containment edge always produces a rejected graph; dropping a zone membership leaves a valid generation |
| Determinism | canonical bytes, content digest, traversal order, diff rendering, disposition lists and rejection codes asserted equal across repeated runs and across independently rebuilt graphs |
| Atomic publication | a batch is all-or-nothing; a rejected batch leaves `HEAD`, `EPOCH` and every generation file byte-identical |
| Crash and partial write | real child processes stopped at three documented fault points on the commit protocol, then reopened: before the commit point (nothing published, temporary state discarded), after the generation rename (the committed generation is recovered at its own number), after the head replace (no recovery needed) |
| Multiprocess authority | an independent process advances the writer epoch and fences a writer held in another process; two processes committing concurrently never tear the store |
| Integrity and recovery | truncation at every prefix rejected; single-byte corruption at many offsets rejected; a corrupt newest generation quarantined and the head moved back to verified evidence; a head pointing past verified evidence moved back; a symbolic-link head quarantined and replaced; recovery is idempotent |
| Malformed input | seeded byte/insert/delete/token fuzzing with repaired digests, random and framed noise, declared-count bombs, oversized documents and lines, invalid UTF-8 and control bytes in labels, path-like and oversized identities, deep hierarchies |
| Concurrency | four reader threads against a publisher, 800 concurrent publish attempts, concurrent read-only handles, and multiple read-write handles fencing each other |
| Independent process | every multiprocess and CLI test runs a genuinely separate operating system process |
| Cancellation | a cancelled commit publishes nothing, leaves no temporary state and does not consume the mutation identity |
| Sanitizer | AddressSanitizer build (MSVC `/fsanitize=address`, RelWithDebInfo): 133/133 tests pass, no reports |
| Warnings | Release and Debug builds with `/W4 /WX /permissive-` (MSVC): zero first-party warnings |
| Package | `cmake --install` then an independent `find_package` consumer, built and run outside the source tree |
| Fresh clone | the committed tree is cloned, configured, built, tested, installed and consumed from scratch |

Hardening defects found and fixed after the first fully green state:

1. pruning re-read and re-verified every retained generation on each commit,
   making publication cost grow with the retention window times the topology
   size — pruning now works from generation file names alone (durable commit at
   67 649 nodes: 19.4 s -> 4.8 s);
2. the idempotency check ran after the base-generation precondition, so a
   legitimate retry of an already-applied batch was refused as stale — the replay
   lookup now happens first;
3. the writer epoch was inside the batch identity digest, so a retry after
   reopening a store looked like different content — the epoch is now excluded
   from the content digest;
4. `PublishedTopology::publish` allocated the shared handle after taking the
   single-publisher flag, so an allocation failure could have left the flag set
   with nothing published — allocation now happens first;
5. `is_canonical_utc_timestamp` accepted impossible calendar days such as
   2023-02-29 — it now delegates to the strict parser;
6. `lowest_common_ancestor` returned the shallowest common ancestor instead of
   the deepest — it now compares the ancestry chains as a common prefix;
7. containment and zone checks depended on edge insertion order, so a legitimate
   import could be refused for the wrong reason — order-dependent checks are now
   deferred to whole-graph validation, which is authoritative;
8. the canonical parser accepted `source test` as equivalent to `source "test"`,
   giving one document two encodings — quoting is now part of the syntax and is
   enforced;
9. generation file names are zero padded, which the canonical "no leading zeros"
   rule rejected, so a store could not see its own files;
10. `PublishedTopology` had no guard against publishing an empty snapshot;
11. a symbolic-link head marker could not be repaired by recovery;
12. the CLI treated any `-`-prefixed token as an option, so a negative option
    value could shift positional arguments.

---

## 12. Benchmarks

`facility_topology_benchmarks` measures **completed** operations. Durable
publication includes writing the generation, flushing it, the atomic rename and
the head replacement. The topologies are **SYNTHETIC**: they are generated by the
benchmark program with a fixed structure and describe no real site. No
accelerator, network, power or cooling hardware was involved in any measurement.

Host: AMD Ryzen 7 9800X3D (16 logical cores), 61.6 GB RAM, Windows, MSVC 19.44,
`Release`, wall-clock `steady_clock`.

| Facility (nodes) | assemble + validate | validate | BFS | DFS | serialize | parse + verify | diff (n changes) | durable create | durable commit | load head |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 74 | 1.4 ms | 0.7 ms | 441 ns/node | 222 ns/node | 6.0 ns/byte | 66 ns/byte | 53 us | 12.7 ms | 7.6 ms | 3.9 ms |
| 1 093 | 21.9 ms | 13.5 ms | 341 ns/node | 192 ns/node | 4.6 ns/byte | 68 ns/byte | 1.3 ms (2 048) | 32.4 ms | 47.1 ms | 3.5 ms |
| 67 649 | 3.02 s | 1.52 s | 528 ns/node | 385 ns/node | 7.4 ns/byte | 229 ns/byte | 70 ms (4 000) | 3.19 s | 6.60 s | 112 ms |

Notes on these numbers:

* integrity was verified in every parse and load measurement;
* durable commit and create costs are dominated by writing and flushing the whole
  canonical document and re-verifying the head, so they scale with topology size;
* repeated reads of an unchanged head reuse a digest-keyed cache of the parsed
  generation and re-verify the bytes on every read: loading the 26.5 MB head of
  the 67 649-node facility takes 112 ms, against 6.1 s for a cold parse;
* point queries (ancestry, neighbours, domain references, descendants) cost
  330 ns to 861 ns each, depending on scale.

Reproduce with `facility_topology_benchmarks` (default) or
`facility_topology_benchmarks --large`.

---

## 13. Genuine limitations

* **Publication is O(n).** Each commit re-verifies the current generation and
  writes the complete next generation. On a 67 649-node facility a durable commit
  takes seconds. This is the price of immutable generations, validation before
  publication and whole-document durability; it is not suitable for
  high-frequency small updates.
* **In-memory storage is ordered maps.** Every lookup and iteration is
  deterministic and logarithmic, but memory use is higher than a compact array
  representation would be.
* **One facility per generation is not enforced, one *site* is assumed.** A
  generation may hold several facility roots, but cross-facility adjacency and
  cross-facility zone membership are refused, and multi-site federation is out of
  scope.
* **Store directories are trusted-local.** The *contents* of a store are
  untrusted and are fully validated, but a process with write access to the store
  directory can delete or replace files; the model defends against corruption and
  stale writers, not against a hostile writer with filesystem access.
* **The POSIX file-locking and flush path is implemented but was not exercised
  on this host.** Validation ran on Windows/MSVC. The POSIX branch uses `flock`
  and `fsync` and is compiled only on non-Windows targets; it is unverified here.
* **No hardware integration and no hardware claims.** Nothing in this repository
  talks to a PDU, UPS, BMS, switch or accelerator. Domain associations are
  references.
* **Timestamps come from an injected clock.** The durable manifest records the
  time the store was given; the library does not synchronise clocks across hosts.
* **`DomainId` is opaque.** This repository does not validate that a referenced
  domain exists in the power or cooling control plane, only that it is declared
  in the same generation.
* **Benchmarks are single-host, single-threaded** apart from the concurrency
  tests, and describe one machine only.

---

## 14. Documentation map

| Document | Contents |
| --- | --- |
| `README.md` | this file: boundary, architecture, semantics, usage, validation, limitations |
| `CONTRIBUTING.md` | licensing of contributions, scope rules, build and test expectations |
| `LICENSE` | Apache License 2.0 |
| `NOTICE` | copyright and third-party notice |
| `examples/` | the public API in use: building, evolving, persisting and consuming a topology |
| `tests/downstream/` | independent consumer of the installed package |

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
