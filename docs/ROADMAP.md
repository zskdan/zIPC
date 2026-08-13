# zIPC roadmap: v0.1 to v1.0

The roadmap consolidates the initial proposal and subsequent requirements for
link identity, cookies, message correlation, QoS, async callbacks, readiness,
Doxygen documentation, production deployment, and the observability and
execution-model workstreams derived from NNG/RPMsg reference tracing.

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

## v0.1.9 — Buffer-offset API

Completed:

- `zipc_buffer_at()` absolute offset access, stable fixed buffer offsets, and
  zero-copy trim semantics; the A->B->C shared-buffer example uses it.
- Topology configuration (static C, string, and file sources) behind one
  canonical registry.
- Guard-page regression test for out-of-bounds payload writes.
- Strict-ownership regression test.

## v0.1.10 — Version API, diagnostics, and build layout

Completed:

- Library version identity aligned to v0.1.10 with `ZIPC_VERSION_*` macros and
  `zipc_version_string()` runtime query.
- Build restructure: `build/examples`, `build/tests`, `build/utilities`,
  `build/libs`, `build/docs`; static `libzipc.a`; `make all` covers every binary
  and the library while `make docs` remains explicit.
- Self-explanatory example/utility output (basic producer, ping-style
  `zipc-ping` with `--interval`, `zipc-stat` diagnostics, chain examples).
- Execution model and observability-by-design principles captured in
  `docs/ARCHITECTURE.md`.
- Reliable incremental header dependencies and deterministic archive rebuilds.
- Correct `zipc-ping` timing units, validated intervals, bounded reply waits,
  and measured packet-loss summaries.

## v0.1.11 — ABI-preserving hardening

Completed:

- Overflow-safe pool geometry, alignment, overlap, and attach metadata
  validation while preserving pool ABI 1 and all public layouts/signatures.
- Correct ABI-1 `ATOMIC32` plus `ATOMIC64` control-memory capability contract.
- Epoch exhaustion, active-epoch recovery, deadline addition, protocol-error
  observability, and strict receive-protection failure hardening.
- Focused Linux regression coverage and synchronized release metadata.

Documented limitations retained beyond v0.2.0:

- ABI 2 compatibility timeout calls cannot override the timeout fixed in an
  opened backend.

## Cross-version workstream: Observability & diagnostics

NNG/RPMsg tracing is treated as zIPC requirements discovery, not throwaway
debugging. The strategic goal: anything difficult to understand in NNG or
RPMsg today is either eliminated in zIPC or made directly observable.

- v0.1.x: trace event ABI; `link_id`/`link_cookie`; send/recv/link lifecycle
  tracepoints; ipctrace compatibility (completed baseline: per-slot trace
  entries, two-layer observability contract in ARCHITECTURE.md).
- v0.2.1+: `message_id`, request correlation, richer protocol tracing, request/reply
  visualization.
- v0.3+: queue/backpressure telemetry, QoS tracing, async/callback tracing,
  backend-specific instrumentation.
- v1.0: stable trace ABI, Wireshark dissector and extcap, Perfetto exporter,
  production diagnostics.

Trace output stays tool-neutral: one normalized zIPC trace event model, with
exporters to CTF, pcapng, and Perfetto rather than four divergent formats.

## Cross-version workstream: Execution model

Architectural principle: no hidden execution resources. zIPC SHALL operate
without internally created threads; an optional shared reactor MAY create a
bounded number of explicitly configured workers; thread creation SHALL never
scale implicitly with links, endpoints, or messages.

- v0.1.x: document the proposed `INLINE`/`POLL`/`REACTOR` model and callback
  policies (completed in ARCHITECTURE.md); verify that the current synchronous
  Linux path creates zero internal threads.
- v0.2.1+: `zipc_get_fd()`/`zipc_process()` event-loop integration; async API
  without implied workers; explicit `ZIPC_CALLBACK_INLINE`/`DEFERRED`/
  `USER_EXECUTOR` policy.
- v0.4: shared request engine, deferred completion queues, dispatcher
  integration for FreeRTOS and kernel workqueues (existing v0.4 scope).

## Cross-version workstream: Benchmarking

Version claims must be measurable ("zIPC v0.2 reduced application-to-backend
latency by 23%"), not impressions. Per message: latency p50/p99/max,
messages/second, CPU, syscalls, context switches, wakeups, copies and bytes
copied, and per-stage latency. Acceptance metrics: threads per process,
threads per socket, stack memory, scheduler latency. Target profile:
2 applications, 20 links -> 2 application threads plus 0 zIPC threads
(INLINE/POLL) or plus one reactor per process (REACTOR).

## Hardening backlog (pull into upcoming releases)

- Define transport-independent duration units and native per-call timed
  operations; ABI 2 currently uses backend-open configuration and cannot
  honestly override it per call.
- Extend publication/acknowledgement semantics for future async cancellation
  and peer-confirmed delivery beyond the synchronous v0.2 published-error
  distinction.
- Harden `/dev/mem` based DT-reserved-memory mappings; production access must
  use a dedicated driver (v0.5 scope).
- Extended soak/fault tests and sanitizer/static-analysis coverage (v0.9).

## v0.2.0 — Buffer identity and correlation lineage

Completed scope:

- Immutable buffer ID and direct parent lineage in pool ABI 2 slot control.
- Allocator:8/session:24/sequence:32 packed identity and concurrent generator.
- Secure platform entropy contract and explicit failure.
- Component namespace 1..254 and exact 256-bit visited set.
- Fixed buffer-aware trace record/hook.
- Allocation API parent argument, lineage accessors, tests, and A->B->C->D child
  example.
- Kernel descriptor `pool_id` synchronization and ABI version update.
- Publication-aware transport status and sender ownership handling.
- Fully quiesced, metadata-independent abandoned `CLAIMING` recovery.

Validation status: no target hardware validation was performed for v0.2.0;
FreeRTOS and bare-metal results are Linux-host stub integrations.

Explicitly not part of v0.2.0: QoS, async APIs/engine, CRC/integrity, payload
typing, protocol flags, flow/request/correlation IDs, link identity/cookie,
feature negotiation, readiness, and eBPF/exporters.

## v0.2.1+ — Incremental protocol semantics

Deferred v0.2 concepts will be delivered incrementally rather than treated as
one release gate: stable link identity/local cookie; request/message/flow
semantics; feature negotiation and rejection; payload/access metadata;
integrity; QoS model; async ownership contract; event-loop integration; and
richer trace/export support. Exact patch/minor allocation follows design review.

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
