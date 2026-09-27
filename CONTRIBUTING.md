# Contributing to Facility Topology

Facility Topology is part of Data Center Control Plane (DCCP), Tranche 1
(Canonical Facility State), and is maintained by Summon Software Labs.

## Licensing of contributions

This project is licensed under the Apache License, Version 2.0 (see `LICENSE`).

By submitting a contribution you agree that it is licensed under the terms of
that license, as described in section 5 of the license text. There is **no
Contributor License Agreement** to sign, and no copyright assignment is
required: you keep the copyright in your contribution and grant the project the
license described in `LICENSE`.

Please do not add co-author trailers or attribution lines that name tools,
assistants or intermediate processes; commit authorship is the responsibility of
the human contributor.

## What belongs in this repository

Facility Topology owns the generation-bound structural graph of the physical
facility: facilities, buildings, halls, rooms, rows, racks, zones, containment,
physical adjacency, references to power and cooling domains, provenance,
immutable snapshots, generation transitions, deterministic serialization and
durable atomic publication.

It deliberately does **not** own:

* detailed asset inventory (Asset Registry);
* rack membership and occupancy policy (Rack Registry);
* stable coordinate or address semantics (Physical Location Registry);
* general cross-service dependency semantics (Facility Dependency Registry);
* network topology, paths or transport (Distributed Fabric Infrastructure);
* accelerator execution, memory, serving or scheduling (Accelerated Systems
  Infrastructure);
* capacity planning, placement optimisation, reservations, electrical
  actuation, cooling control, maintenance orchestration, tenancy policy,
  incident response, observability dashboards or multi-site federation.

External ASI and DFI objects may be referenced only through stable opaque
identifiers or typed references. Do not reimplement their semantics here, and
do not add hardware integration that cannot be exercised.

## Building and testing

The project is a portable C++20 CMake project. From a clean checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements:

* CMake 3.20 or newer;
* a C++20 compiler (MSVC 19.30+, GCC 11+, or Clang 14+);
* no third-party dependencies.

All first-party code must build with zero warnings. The default configuration
treats warnings as errors (`FACILITY_TOPOLOGY_WARNINGS_AS_ERRORS=ON`); fix the
cause rather than suppressing the warning.

## Expectations for a change

* **Correctness before coverage.** A change to authoritative state should come
  with proof: a test that fails before the change and passes after it.
* **Test the boundary, not just the happy path.** Persistent state, canonical
  documents, CLI arguments and imported records are untrusted input. Malformed,
  truncated, oversized, reordered, duplicated and corrupted inputs must be
  rejected with a stable error code.
* **Tests must terminate on their own.** A test that hangs is a defect to
  diagnose and repair, not to work around.
* **Determinism.** Where the library claims deterministic behaviour — canonical
  ordering, serialization, traversal, diff, rejection — the behaviour must be
  encoded in a test, not left to incidental container behaviour.
* **Stable vocabulary.** Error codes and canonical tokens are a public
  contract. Extend them; never renumber or repurpose an existing value.
* **Generations and authority.** Never add a mutation path that bypasses the
  generation precondition, the writer epoch or whole-graph validation.
* **Bounded resources.** Validate declared sizes, counts and limits before
  allocating. Bound traversal depth, recursion and document sizes.
* **Durability discipline.** Authoritative state changes only through the
  documented protocol: plan, validate, write a temporary generation, verify
  integrity, atomically publish, then retire superseded temporary state.
* **Comments explain intent.** Describe why a constraint exists; do not narrate
  the code.

## Style

* C++20, standard library only.
* Formatting follows the surrounding code: two-space indentation, 120-column
  target, `snake_case` for functions and variables, `PascalCase` for types,
  trailing `_` for private data members.
* Public headers live under `include/dccp/facility_topology/` and must be
  self-contained.
* Every file starts with the SPDX identifier and the copyright line:

  ```
  // SPDX-License-Identifier: Apache-2.0
  // Copyright 2026 Summon Software Labs.
  ```

## Reporting a problem

Open an issue with a minimal reproduction: the exact command, the exact input
(for a canonical document, the smallest document that shows the problem), the
observed result and the expected result. If a property test failed, include the
seed — every randomized test prints the seed it ran with.
