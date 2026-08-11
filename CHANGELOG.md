# Changelog

## v0.1.19

- Release of the buffer-offset API work (previously published as v0.1.9) as v0.1.19.
- Includes `zipc_buffer_at(buffer, offset, length)`, stable fixed buffer offsets, zero-copy `trim_front`/`trim_back`, topology configuration, guard-page, strict-ownership, and simplified-API regression tests.

## v0.1.9

- Added `zipc_buffer_at(buffer, offset, length)` for ownership-checked absolute access within complete buffer storage.
- Defined public buffer offsets relative to buffer storage offset zero, independent of the current logical data window.
- Documented and tested zero-copy `trim_front`/`trim_back` semantics and stable fixed offsets across trims.
- Kept `zipc_buffer_capacity()`, `zipc_buffer_resize()`, and `zipc_buffer_const_data()` only as v0.x compatibility APIs.
- Updated the A->B->C shared-buffer example to use `zipc_buffer_at()` instead of application pointer arithmetic.
- Updated public Doxygen, API documentation, README, examples, and regression tests.

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
