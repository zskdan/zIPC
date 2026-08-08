# zIPC changelog

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
