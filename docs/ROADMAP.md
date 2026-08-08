# zIPC roadmap: v0.1 to v1.0

The roadmap consolidates the initial proposal and subsequent requirements for
link identity, cookies, message correlation, QoS, async callbacks, readiness,
Doxygen documentation, and production deployment.

## v0.1 — Resilience foundation

Completed:

- Component registration, epochs, heartbeats, and restart detection foundation.
- Explicit orphan-slot recovery.
- Hop limits and absolute deadlines.
- Timeout-oriented API entry points.
- Fixed-depth per-slot tracing.
- `zipc-stat` diagnostics.

## v0.1.1 — Target-adapter integration validation

Completed:

- Host-buildable FreeRTOS integration using the real adapter plus stubs.
- Host-buildable bare-metal integration using the real adapter plus simulated IPI.
- Integration targets included in `make test`.

## v0.2 — Protocol identity, semantics, QoS model, async contract, and documentation

- Stable logical `link_id`.
- Local application-owned link cookie.
- End-to-end `correlation_id` and optional persistent `flow_id`.
- Payload type, protocol flags, priority, traffic class, and access intent.
- Optional CRC32/integrity metadata.
- Required/supported feature masks and version negotiation.
- Explicit protocol rejection/error semantics.
- Initial QoS model: class, priority, depth, max inflight, latency target,
  deadline, and drop policy.
- Async public contract: request IDs, request cookies, completion events,
  cancellation API, delivery states, callback contexts, and ownership rules.
- Complete Doxygen comments for public headers and examples.
- `Doxyfile` and `make docs` producing `build/docs/html/index.html`.

## v0.3 — Advanced buffer model

- Scatter-gather segments.
- Multi-slot messages and chained allocation/release.
- Header/payload split helpers.
- External DMA-buffer import/export.
- Buffer capability metadata: physical contiguity, DMA access, cacheability,
  security domain, read-only intent, device memory.
- Segment-aware address and future cache-sync helpers.

## v0.4 — Async engine, transport contracts, and QoS enforcement phase 1

- Implement async send/receive and request cancellation.
- Shared internal request engine for synchronous and asynchronous calls.
- Deferred completion queues and platform dispatch contexts.
- Linux poll/epoll wait handles and explicit dispatch API.
- FreeRTOS dispatcher task and Linux kernel workqueue integration.
- Transport capability discovery: reliability, ordering, depth, polling,
  timeout support, ISR safety, descriptor size, and backpressure behavior.
- Uniform try/timed send and receive semantics.
- Enforce queue depth, max inflight, priority where supported, deadlines,
  backpressure, and drop policy.
- Endpoint lifecycle and reconnect notifications.

## v0.5 — Cache, DMA, and heterogeneous coherency

- `zipc_buffer_sync_for_cpu()` and `zipc_buffer_sync_for_device()`.
- Explicit clean/invalidate adapters where needed.
- Linux DMA API and reserved-memory driver integration.
- R5 FreeRTOS and bare-metal cache adapters.
- Coherent, I/O-coherent, software-maintained, and uncached policies.
- PL ring/IRQ and DMA completion integration.
- BRAM payload plus DDR metadata reference design.
- Physical-address and IOVA accessors where valid.

## v0.6 — Topology, readiness, peer lifecycle, and health

- Link states: down, starting, ready, degraded, failed.
- Peer states: unknown, booting, ready, alive, stopping, dead, restarted.
- `zipc_link_wait_ready()`, readiness queries, and readiness timeouts.
- Peer heartbeat/liveness queries and epoch/restart handling.
- Readiness handshake for ABI/features, identities, memory layout, transport,
  cache policy, and QoS compatibility.
- Persistent link callbacks for ready, down, peer restarted, recovery,
  QoS violation, and error.
- Declarative graph/topology API.
- Validation of duplicate IDs, missing peers, loops, reachability, transport
  compatibility, and QoS conflicts.
- Static platform configuration generation.

## v0.7 — Security and isolation

- Production FF-A and SMC integration.
- Secure memory share/lend/reclaim lifecycle.
- Secure-side validation of untrusted metadata.
- Secure and non-secure pool policy.
- Per-component/per-link authorization and payload-type policy.
- Replay protection and secure epoch handling.
- Xen guest restart and event-channel lifecycle hardening.
- XMPU/XPPU integration guidance.

## v0.8 — Observability, QoS metrics, and tooling

- Per-link/component counters for packets, bytes, drops, timeouts, recoveries,
  occupancy, inflight requests, deadline misses, QoS violations, and restarts.
- Latency histograms for allocation, send, transport, per-hop, end-to-end, and
  callback dispatch.
- Configurable/external trace buffers and binary trace export.
- Correlation-ID and link-ID filtering.
- JSON, CSV, Prometheus, and OpenTelemetry-oriented output.
- Expanded `zipc-stat`, `zipc-ping`, `zipc-membench`, and `zipc-packetrate`.
- Fault-injection and topology-inspection utilities.
- QoS phase 2: multiple priority rings, reserved slots, weighted scheduling,
  admission control, and detailed violation metrics.

## v0.9 — Production hardening and release candidate

- Concurrency, wraparound, saturation, malformed-input, crash, reset,
  reconnect, cache-corruption, and long-duration soak tests.
- ASan, UBSan, TSan where applicable, static analysis, fuzzing, KUnit,
  FreeRTOS tests, hardware-in-loop CI, and coding-standard checks.
- 32/64-bit, endianness, structure-layout, and ABI compatibility tests.
- CMake, pkg-config, Yocto/PetaLinux recipes, kernel build integration, and
  package generation.
- Security review and threat model.
- Documentation and ABI freeze candidate.

## v1.0 — Stable supported release

- Stable public C API and shared-memory ABI.
- Stable handle and compatibility policy.
- Supported migration and upgrade path.
- Production Linux userspace and kernel support.
- Supported FreeRTOS, bare-metal, Xen, and A53/R5/PL reference deployments.
- Stable synchronous, asynchronous, callback, readiness, recovery, and QoS
  semantics.
- Complete Doxygen reference and integration guides.
- Complete integration, stress, security, and hardware-validation matrix.


## Backend terminology

Use the canonical three-role model: payload backend, descriptor backend, and event backend. See `docs/BACKENDS.md`.
