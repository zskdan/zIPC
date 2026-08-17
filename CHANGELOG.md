# Changelog

## v0.2.0

- Extended `zipc-packetrate` with configurable zero-copy relay chains, dynamic
  payload-sized pool geometry, automatic or explicit slot sizing, bounded
  warm-up, and end-to-end versus aggregate transfer-rate reporting.
- Added a repeatable packet-rate scaling analyzer with randomized relay sweeps,
  zero-payload controls, CPU/context-switch/RSS metrics, CSV data, and Markdown
  reporting.
- Added a focused buffer API walkthrough covering headroom/tailroom, append,
  prepend, absolute offset access, trimming, bounds checks, and ownership
  invalidation.
- Added immutable 64-bit buffer IDs and parent lineage to pool ABI 2 slot
  control metadata without expanding `zipc_message_t` descriptors.
- Added the packed allocator/session/sequence identity layout, one lazy
  process-global generator per component ID, 32-bit-atomic rollover, explicit
  entropy failure, and secure platform entropy adapters.
- Changed allocation APIs to accept an optional parent buffer; root allocations
  use parent ID zero, child allocation preserves the parent, and relays preserve
  both IDs.
- Expanded the component namespace to 256 entries with usable IDs 1..254 and
  centralized reserved-ID validation across core, links, and topology.
- Replaced the 64-bit visited mask with an exact owner-protected eight-word
  visited set, retaining 64 as the independent topology link capacity.
- Added a fixed synchronous trace record/hook carrying timestamp, event, buffer
  lineage, handle, pool, component, and transfer sequence.
- Retained the honest atomic64 control-memory requirement for existing shared
  counters and lifecycle timestamps; the local identity generator itself uses
  only 32-bit atomics.
- Synchronized the Linux kernel descriptor `pool_id` layout and kernel ABI 2,
  and updated the A->B->C->D chain example to create five child buffers.
- Added deterministic rollover/entropy seams and tests for packing, lineage,
  relay preservation, concurrency, reserved IDs, visited words, tracing, and
  ABI-1 rejection.
- Added the ABI-2 transient `CLAIMING` state so allocation and receive publish
  `OWNED` only after metadata and local-view construction complete; recovery
  ignores in-progress claims and failed claims free directly.
- Added explicit recovery for claims abandoned by a crash before claimant
  identity can be fully published; the final contract requires full pool
  quiescence and reclaims all remaining claims without an age threshold.
- Strengthened identity rollover with a 32-bit atomic active-issuer gate that
  rotation drains before replacing the session/resetting sequence, including a
  controlled concurrent rollover regression.
- Made Linux kernel entropy readiness-aware with `get_random_bytes_wait()`,
  tightened trace-hook reentrancy/configuration documentation, and made the
  lineage example validate and report every child ID, parent, size, and payload.
- Added `ZIPC_ERR_TRANSPORT_PUBLISHED` across public and kernel status enums.
  Backends now distinguish pre-publication failure, which permits sender
  rollback, from post-publication notification/callback failure, which leaves
  the slot in `TRANSFER`, invalidates sender ownership, and returns the error.
- Simplified `zipc_pool_recover_claiming()` to reclaim every `CLAIMING` slot
  only under full external pool quiescence. Recovery no longer uses claim age
  or partial claimant metadata, attributes no component recovery, and restores
  `CLAIMING` for retry if payload protection fails.
- Added regressions for unpublished send rollback, published-error ownership
  consumption, zero/partial-metadata claims, owner-recovery exclusion, and
  retryable claiming-recovery protection failure.
- Ordered strict payload protection inside ownership transitions: release and
  recovery revoke access before `FREE`, while unpublished-send rollback restores
  access before `OWNED`; failed restoration leaves a recoverable claim instead
  of returning unsafe ownership.
- Defined owner and abandoned-claim recovery as pool-quiesced administrative
  operations, preventing recovery probes from racing a receiver after its sole
  transfer descriptor has been consumed.
- Made the 32-bit slot generation atomic so stale-handle validation remains
  race-free while another component allocates and reuses the slot.
- Hardened owner recovery to acquire each candidate state before reading
  owner-protected metadata and to restore nonmatching owner, epoch, or age
  candidates without reclaiming a reused slot.
- Made transfer preparation acquire `CLAIMING`, revoke strict payload access,
  and complete metadata and tracing before publishing `TRANSFER`; unpublished
  transport failure restores metadata and payload access before `OWNED`.
- Enabled strict payload access for successful low-level
  `zipc_buffer_allocate()` calls as well as high-level allocation.
- No target hardware validation was performed for v0.2.0. Linux tests and the
  FreeRTOS/bare-metal host-stub integrations are not hardware validation.

## v0.1.11

- Hardened pool geometry with overflow-safe alignment and size arithmetic,
  control/payload address alignment checks, same-memory overlap rejection, and
  exact ABI-1 `header_size`/`controls_offset` validation before attach derives
  slot-control or payload pointers.
- Corrected the ABI-1 control-memory contract to require CPU read/write plus
  `ZIPC_MEM_CAP_ATOMIC32` and `ZIPC_MEM_CAP_ATOMIC64`; ABI 1 actively uses
  shared `_Atomic uint64_t` fields.
- Prevented component epoch zero on wrap, rejected exhausted epochs, rejected
  recovery of a component's currently active exact epoch, and saturated
  overflowing relative deadlines to `ZIPC_DEADLINE_NONE`.
- Counted prepare/claim protocol failures and emitted per-slot
  `ZIPC_TRACE_ERROR` only after ownership is validated; claim pre-validation
  failures update the atomic counter without racing owner-protected trace data.
- Released a newly claimed slot when strict-protection receive setup fails so
  the failure does not silently leave an orphaned `OWNED` slot.
- Documented that ABI-1 timeout compatibility calls use the timeout configured
  when the backend is opened; their per-call argument cannot override it.
- Added focused Linux hardening regression coverage without changing pool ABI 1,
  public signatures, or public/shared structure layouts.

## v0.1.10

- Added library version API: `ZIPC_VERSION_MAJOR`/`MINOR`/`PATCH`/`STRING`
  macros and `zipc_version_string()`; synchronized v0.1.10 identity across
  VERSION, README, Doxyfile, AGENTS, public API, tests, and handoff docs.
- Added producer-side output to the basic example, ping-style output and
  `--interval` option to `zipc-ping`, clearer `zipc-stat` diagnostics,
  sender-side transfer prints in the Linux integration test, lifecycle/PID
  prints in the ready-linux5 example, and self-explanatory shared-buffer-chain
  output.
- Restructured Linux build outputs: `build/examples`, `build/tests`,
  `build/utilities`, `build/libs`, `build/docs`.
- Added `make libs` producing `build/libs/libzipc.a`; all Linux examples,
  tests, and utilities link against it.
- Captured the no-hidden-threads execution model, observability-by-design,
  and identity contract in `docs/ARCHITECTURE.md`; consolidated the roadmap
  with observability, execution-model, and benchmarking workstreams plus a
  hardening backlog.
- Corrected `zipc-ping` latency units, input validation, reply timeouts, packet
  loss statistics, and forked output buffering; corrected `zipc-stat`
  diagnostics and added utility error-path checks.
- Added generated header dependencies for reliable incremental builds and
  deterministic static-library recreation.

## v0.1.9

- Release of the buffer-offset API work as v0.1.9.
- Includes `zipc_buffer_at(buffer, offset, length)`, stable fixed buffer offsets, zero-copy `trim_front`/`trim_back`, topology configuration, guard-page, strict-ownership, and simplified-API regression tests.

## v0.1.8

- Added one canonical declarative topology model (`zipc_topology_config_t`).
- Added named runtime pool/transport resource bindings.
- Added static/header topology registration through `zipc_topology_register_config()`.
- Added INI topology parsing from memory with `zipc_topology_load_string()`.
- Added hosted/Linux file loading with `zipc_topology_load_file()`.
- Added explicit duplicate policies: reject, extend and override.
- Added common topology validation and process-local reset.
- Updated the shared-buffer chain example to run from either `zipc_config.h` or `zipc.conf`.
- Added topology-source regression coverage and `docs/TOPOLOGY.md`.

## 0.1.7

- Simplified application API for NNG migration: `zipc_buffer_alloc`, `zipc_recv`, link-independent `zipc_buffer_release`, and copy helpers.
- Made `zipc_buffer_t` opaque while retaining stack allocation.
- Successful send/release now invalidate the local buffer object; misuse is detected.
- Added named static topology registration and `zipc_link_open`.
- Added stable pool IDs to descriptors and diagnostic buffer accessors.
- Added optional `ZIPC_POOL_F_STRICT_OWNERSHIP` Linux page-permission enforcement.
- Kept guard pages as an independent optional overflow detector.
- Added A->B->C same-buffer chain example, NNG migration guide, ownership guide, API guide, Doxygen configuration, and new API/protection tests.
- Converted shipped examples/utilities to the simplified API; compatibility wrappers remain for v0.x.

# zIPC changelog

## v0.1.6

- Added optional `ZIPC_POOL_F_GUARD_PAGES` for Linux POSIX shared-memory payload pools.
- Guarded pools insert one `PROT_NONE` page after every page-aligned payload slot so a linear overrun faults before reaching the next slot.
- Added `ZIPC_MEM_CAP_PAGE_PROTECT`, page-size/protection platform hooks, and guarded stride/payload sizing helpers.
- Added `tests/guard-pages-linux.c` and the `make guard-pages` regression target.
- Guard pages remain independent of links and do not replace zIPC ownership checks; directly addressing another valid slot is still possible.

## v0.1.5

- Changed the shared-memory polling event backend from cooperative yielding to busy spinning with an architecture-specific CPU-relax instruction.
- Added `poll_timeout_ns` to the transport configuration. A value of `0` waits forever; a non-zero value returns `ZIPC_ERR_TIMEOUT` after a monotonic deadline.
- Added regression coverage for timed polling.

# Changelog

## v0.1.4

- Added `ZIPC_EVENT_BACKEND_SHM_POLLING`.
- Added Linux `ZIPC_TRANSPORT_SHM_RING_POLLING`, combining a shared SPSC descriptor ring with direct shared-memory polling.
- Updated backend-role documentation and regression coverage.

# zIPC changelog

## v0.1.2

- Added `ZIPC_SHM_PREALLOCATED` for caller-owned, already mapped memory.
- Added FreeRTOS and bare-metal platform support for static arrays, linker sections, OCRAM, TCM, and BSP-provided regions.
- Updated the FreeRTOS integration test to exercise the preallocated backend.
- Documented global static array, linker-script section, and OCRAM configurations.

## v0.1.1

- Added `tests/integration-freertos.c` using the real FreeRTOS platform adapter with host-side FreeRTOS/OpenAMP stubs.
- Added `tests/integration-baremetal.c` using the real bare-metal platform adapter and a simulated IPI doorbell.
- Added `integration-freertos`, `integration-baremetal`, and `integration-targets` Makefile targets.
- Extended `make test` to execute both target-oriented integration tests.
- Clarified that these host harnesses validate protocol/platform integration but do not replace BSP or hardware validation.

# Changelog

## zIPC v0.1

- Added component registration, epochs, heartbeats, unregister, snapshots, and orphan-slot recovery.
- Added hop-limit and absolute-deadline enforcement for looped chains.
- Added timeout-oriented high-level send/receive entry points.
- Added fixed-depth per-slot trace history.
- Added `zipc-stat` and a resilience regression test.
- Added the staged roadmap through v1.0.
- Incremented the on-memory pool ABI to 1.

## zIPC v0.0

- Initial architecture prototype with shared slot pools, generation handles, split control/payload memory, multiple platform adapters, heterogeneous transports, ready-to-play examples, and benchmark utilities.
