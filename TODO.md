# zIPC active TODO

This file is the maintained checklist for the active milestone. The canonical
release strategy and postponed-feature registry are in
[`docs/ROADMAP.md`](docs/ROADMAP.md).

## Status

- Current release: v0.3.0.
- Product status: experimental; not delivered or deployed.
- Active milestone: v0.4.0, core observability and debuggability.
- First delivery target: v1.0.0, Linux userspace production.
- Active production profile: homogeneous Linux using POSIX shared memory and
  SHM ring polling/eventfd.

## v0.4.0 objective

Make protocol and platform failures self-explanatory before extending the
protocol. Operators must be able to identify the failed operation, status,
component, link, backend, buffer, and platform cause without attaching a
debugger.

## Active work

- [ ] Inventory existing statuses, counters, trace events, and platform error
  mappings; record gaps and compatibility impact.
- [ ] Design a canonical symbolic status API and structured platform error
  detail without introducing hidden allocation or execution resources.
- [ ] Design coherent snapshots for pool, component, link, ring, and slot state.
- [ ] Define a versioned normalized event model with operation, status,
  component, link, backend, buffer identity, process/thread, and timing context.
- [ ] Define trace-loss and unavailable/inconsistent-snapshot reporting.
- [ ] Extend `zipc-stat` to expose all existing counters and relevant identity,
  ownership, lifecycle, transfer, and error fields.
- [ ] Add machine-readable JSON diagnostics.
- [ ] Add focused tests for symbolic errors, platform causes, coherent snapshots,
  event fields, trace loss, and JSON output.
- [ ] Document the event model, snapshot consistency, overhead constraints, and
  operator workflows.
- [ ] Run `make clean`, `make test`, `make utilities`, and
  `make integration-targets` before release.

## v0.4.0 exit criteria

- [ ] Every supported Linux failure path has a symbolic operation and status.
- [ ] Actionable Linux platform causes are retained where available.
- [ ] Diagnostic snapshots cannot silently combine incompatible ownership
  transitions.
- [ ] Trace loss is visible.
- [ ] Human-readable and JSON outputs contain equivalent diagnostic meaning.
- [ ] Observability-disabled overhead and memory impact are measured.
- [ ] Public API/ABI changes, if any, are documented and versioned.
- [ ] Tests, Doxygen, changelog, roadmap status, and validation evidence are
  complete.

## Next milestones

- [ ] v0.5.0: Prometheus, eBPF, Wireshark, and Perfetto integrations.
- [ ] v0.6.0: automated testing and CI.
- [ ] v0.7.0: measured coverage, sanitizers, and static analysis.
- [ ] v0.8.0: documentation and Linux contract closure.
- [ ] v0.9.0: Linux userspace release candidate.
- [ ] v1.0.0: delivered and deployed Linux userspace product.

v1.0.0 cannot release until every v0.4-v0.9 gate passes, including mandatory
Prometheus, eBPF/USDT, Wireshark, and Perfetto integration.

## Known Linux release blockers

- [ ] Reconcile an already-published descriptor correctly when the ring is full.
- [ ] Define bounded wait, shutdown, interruption, backpressure, and peer-down
  behavior for the supported Linux profile. The unified native per-call timing
  and async cancellation APIs remain scheduled for v2.0-v2.1.
- [ ] Prevent duplicate buffer identity after `fork()`.
- [ ] Make topology registration transactional and enforce unique link IDs.
- [ ] Validate supported atomic, ABI layout, size, alignment, architecture, and
  endianness assumptions.
- [ ] Add install/pkg-config/downstream-consumer validation.
- [ ] Complete saturation, crash, restart, shutdown, and soak qualification.

These blockers are scheduled for v0.6-v0.9 but may be fixed earlier. Correctness
and security fixes do not wait for their planned milestone.

## Recently completed

- [x] Added Sphinx/Breathe documentation builds and GitHub Pages deployment
  while retaining standalone Doxygen HTML generation.
- [x] v0.2.0 buffer identity and parent lineage.
- [x] v0.3.0 supervisorless Linux SHM ring relay restart recovery.
- [x] Named recovery scenarios, normal/verbose/quiet output, timestamped stage
  breakdowns, and actionable failure-state dumps.
- [x] Consolidated the product roadmap around Linux v1.0 delivery, followed by
  FreeRTOS/RPMsg, QoS, hardening/security, networking, and kernel/DMA.

## Blockers

- No active implementation blocker is recorded.
- Target hardware and deployment applications must be selected before their
  corresponding qualification milestones begin.

## Maintenance rules

- Keep only the active milestone detailed here; keep long-range detail in
  `docs/ROADMAP.md`.
- Update this file in every pull request that starts, completes, blocks, moves,
  or discovers active work.
- Mark a task complete only after code, tests, documentation, and validation are
  complete.
- Preserve postponed features in `docs/ROADMAP.md`; never silently delete them.
- Release pull requests must update the current release and active milestone and synchronize
  `VERSION`, `CHANGELOG.md`, `README.md`, `MANIFEST.txt`, and the roadmap.
