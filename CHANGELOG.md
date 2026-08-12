# Changelog

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
