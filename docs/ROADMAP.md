# zIPC delivery roadmap

This is the canonical product roadmap for zIPC. `TODO.md` is the live checklist
for the active milestone. A feature may be postponed or moved, but it must not
be silently removed: every postponed feature remains listed here with a target
version family.

## Current status

- Current release: v0.3.0.
- Shared-memory pool ABI: 3.
- Status: experimental prototype; not delivered or deployed as a supported
  product.
- Validated scope: Linux-host tests, including buffer identity and lineage plus
  supervisorless relay recovery for SHM ring eventfd and polling.
- Target hardware has not been validated. FreeRTOS and bare-metal tests use
  Linux-host stubs or simulation.
- Public API and shared-memory ABI may change throughout v0.x.

## Delivery policy

- All remaining v0.x releases prepare one narrow Linux userspace product.
- v1.0.0 is the first delivered and deployable release for Linux applications.
- Minor releases within a stable major version are backward compatible.
- A new major version may revise contracts, but requires a migration guide.
- The current and previous production major lines receive critical correctness
  and security fixes.
- Completion requires implementation, tests, measured evidence, documentation,
  and an explicit support statement.
- Simulated, host-tested, hardware-tested, and production-supported are distinct
  validation levels.
- Critical correctness and security defects are fixed when found; they do not
  wait for their roadmap phase.

## Priority order

The delivery order is:

1. Observability and debuggability.
2. Testing.
3. Coverage and analysis.
4. Documentation and contract closure.
5. Linux userspace production readiness.
6. FreeRTOS production readiness.
7. Linux A53 to FreeRTOS R5 RPMsg production readiness.
8. QoS and execution model.
9. Broad hardening and security.
10. Network production readiness.
11. Linux kernel, DMA, and hardware production readiness.

## Cross-cutting benchmarking

Every production gate records latency p50/p99/max, messages per second, CPU,
syscalls, context switches, wakeups, copies/bytes copied, memory/stack use, and
per-stage latency where applicable. Claims must reference reproducible evidence,
not impressions. The core continues to create no hidden threads; configured
reactor/dispatcher resources are measured separately.

## Version map

| Version | Product milestone |
| --- | --- |
| v0.4.0 | Core observability and debuggability |
| v0.5.0 | Prometheus, eBPF, Wireshark, and Perfetto integrations |
| v0.6.0 | Automated testing and CI |
| v0.7.0 | Coverage, sanitizers, and static analysis |
| v0.8.0 | Documentation, contracts, and support matrix |
| v0.9.0 | Linux userspace release candidate |
| v0.9.x | Soak testing, deployment pilots, and stabilization |
| v1.0.0 | Linux userspace production delivery |
| v1.1.0 | FreeRTOS production delivery |
| v1.2.0 | Linux A53 to FreeRTOS R5 RPMsg production delivery |
| v2.0.0 | QoS phase 1 |
| v2.1.0 | Async request engine and event-loop integration |
| v2.2.0 | QoS phase 2 and advanced scheduling |
| v3.0.0 | Broad production hardening |
| v3.1.0 | Security foundation |
| v3.2.0 | Secure platform integration |
| v3.3.0 | Xen and isolation expansion |
| v4.0.0 | Network wire protocol and TCP |
| v4.1.0 | TCP/TLS and mutual TLS |
| v4.2.0 | Unreliable UDP datagram profile |
| v4.3.0 | Reliable UDP profile |
| v4.4.x | QUIC, multicast, discovery, and RDMA expansion |
| v5.0.0 | Linux kernel production delivery |
| v5.1.0 | DMA and cache coherency |
| v5.2.0 | A53/R5/PL hardware production delivery |
| v5.3.0 | Advanced buffer model |
| v5.4.x | Additional kernel and hardware transports |

## v0.x - Linux userspace preparation

No v0.x milestone is a production delivery. Non-Linux adapters may receive
compile checks and correctness fixes, but their production qualification starts
after v1.0.0.

### v0.4.0 - Core observability and debuggability

- Add a canonical symbolic status API and structured platform error causes.
- Preserve actionable Linux failure context instead of collapsing every cause
  into a generic platform error.
- Define coherent snapshots for pools, components, links, rings, and slots.
- Define one versioned normalized event model carrying operation, status,
  component, link, backend, buffer identity, process/thread, and timing context.
- Report trace loss and inconsistent/unavailable snapshots explicitly.
- Expose all existing counters and relevant identity/ownership fields through
  `zipc-stat`.
- Add machine-readable JSON diagnostics.
- Keep instrumentation optional, bounded, allocation-aware, and free of hidden
  execution resources in the core library.

Exit gate: routine and fault-test failures identify where, why, and on which
component/link/backend they occurred, and diagnostic snapshots cannot silently
mix incompatible ownership transitions.

### v0.5.0 - Production observability integrations

- Export operational counters and histograms in Prometheus format without
  adding a hidden server thread to the core library.
- Add CSV export for offline analysis.
- Add Linux USDT tracepoints and supported eBPF tooling.
- Export normalized events as pcapng and provide Wireshark extcap/dissector
  support.
- Export protocol and recovery timelines to Perfetto.
- Test exporter correctness, event loss, sampling, and disabled/enabled overhead.
- Preserve one normalized source event model rather than implementing divergent
  semantics per exporter.

OpenTelemetry is retained for v4 cross-host distributed tracing. Prometheus
covers the initial operational metrics; Perfetto, Wireshark, and eBPF cover the
initial local Linux trace-analysis requirements. CTF and additional exporters
remain in the postponed registry.

### v0.6.0 - Automated testing and CI

- Run clean GCC and Clang builds and correctness tests in CI.
- Separate fast correctness, integration/fault, and benchmark targets.
- Apply outer timeouts and reliable resource/process cleanup to every test.
- Add negative tests for public error results and backend failure mappings.
- Add concurrent allocation/reuse, saturation, wraparound, lifecycle, shutdown,
  topology, and recovery tests.
- Expand fault-injection and topology-inspection utilities.
- Add regressions for full-ring duplicate recovery, fork-safe identity,
  transactional topology registration, peer death, interruption, and blocked
  shutdown.
- Compile-check FreeRTOS, bare-metal, kernel, and Xen adapters without implying
  runtime or production support.

### v0.7.0 - Coverage and analysis

- Publish GCC and LLVM line/branch coverage in CI.
- Track core and platform/backend coverage separately.
- Establish a reviewed baseline and no-regression ratchet.
- Run ASan and UBSan regularly.
- Run targeted TSan jobs for identity, allocation, lifecycle, ownership, and
  recovery concurrency.
- Add static analysis with reviewed suppressions.
- Keep uncovered production paths visible rather than inheriting Linux coverage
  for other platforms.

### v0.8.0 - Documentation and contract closure

- Document every public API and make missing Doxygen documentation fail CI.
- Publish an accurate capability matrix using production, hardware-tested,
  host-tested, simulated, skeleton, blueprint, and unsupported classifications.
- Define ownership, concurrency, timeout, cancellation, shutdown, recovery, and
  error contracts.
- Document supported ABI, architecture, atomic, endianness, layout, and cache
  assumptions.
- Add installation, deployment, diagnostics, recovery, upgrade, rollback, and
  troubleshooting guides.
- Add operator guides for `zipc-stat`, Prometheus, eBPF, Wireshark, and Perfetto.
- Publish the proposed v1 compatibility and support policy.

### v0.9.0 - Linux userspace release candidate

The initial supported profile is deliberately narrow:

- Homogeneous x86-64 and/or AArch64 Linux processes.
- POSIX shared memory.
- SHM ring polling and eventfd descriptor/event transports.
- Synchronous operations with bounded waits and explicit shutdown.
- Buffer identity and lineage.
- Supervisorless relay restart recovery.

Required closure:

- Reconcile an already-published duplicate correctly even when a ring is full.
- Distinguish timeout, interruption, backpressure, peer-down, and shutdown.
- Make buffer identity safe across `fork()`.
- Make topology registration transactional and enforce unique link IDs.
- Validate process-shared atomic requirements and exact ABI size/alignment/layout.
- Provide `DESTDIR` installation, public headers, library artifacts, license, and
  pkg-config metadata.
- Build a downstream consumer using only installed files.
- Pass saturation, crash, restart, shutdown, and long-duration soak tests.
- Freeze the candidate Linux API and shared-memory ABI.

### v0.9.x - Stabilization and pilots

- Accept release-candidate fixes rather than speculative features.
- Integrate at least one real Linux application.
- Record performance, CPU, memory, syscall, wakeup, and tail-latency baselines.
- Validate installation, deployment, upgrade, rollback, and diagnostics.
- Produce reproducible release artifacts.

## v1.x - Delivered local platforms

### v1.0.0 - Linux userspace production

v1.0.0 is delivered only when every v0.4-v0.9 gate passes and the scoped Linux
profile is stable, installable, observable, documented, and validated by a real
application integration. Prometheus, eBPF/USDT, Wireshark extcap/dissection, and
Perfetto export are mandatory parts of this gate. It does not claim FreeRTOS,
RPMsg, network, kernel, Xen, or DMA production readiness.

### v1.1.0 - FreeRTOS production

The first supported profile uses a selected R5 board, BSP, compiler, and
FreeRTOS release with static/preallocated resources, Normal non-cacheable shared
memory, and the FreeRTOS queue transport.

- Define deterministic memory, stack, queue, and execution limits.
- Qualify timeout, full-queue, ISR deferral, allocation-failure, reset, and
  restart behavior.
- Validate production entropy and required atomic operations.
- Publish WCET and resource evidence.
- Run hardware-in-loop burst, saturation, reset, and long-soak tests.
- Preserve zero hidden tasks in the core; any dispatcher is explicit.

Task notifications, cached memory, advanced recovery, IPI, PL IRQ, SMC, and FF-A
remain scheduled for later v1.x, v3.x, or v5.x qualification.

### v1.2.0 - Linux A53 to FreeRTOS R5 RPMsg production

- Qualify Linux remoteproc/RPMsg communicating with FreeRTOS/OpenAMP on R5.
- Define nameservice, endpoint negotiation, lifecycle, and teardown.
- Define bounded send/receive, backpressure, remote restart, endpoint loss, and
  reconnection.
- Validate shared payload memory, ownership, cache policy, descriptor versions,
  and malformed input.
- Add A53-to-R5 hardware-in-loop burst, saturation, restart, and soak tests.
- Integrate Prometheus metrics and applicable eBPF, Wireshark, and Perfetto data.

R5-to-R5 and bare-metal/OpenAMP profiles remain planned for v1.2.x.

## v2.x - QoS and execution model

### v2.0.0 - QoS phase 1

- Discover transport capabilities: reliability, ordering, depth, polling,
  timeout, ISR safety, descriptor size, and backpressure.
- Define link class, priority, traffic class, queue depth, maximum inflight,
  latency target, deadline, backpressure, and drop policy.
- Negotiate or reject unsupported configurations rather than silently degrading.
- Enforce the common subset on Linux, FreeRTOS, and RPMsg.
- Export occupancy, drops, deadline misses, and QoS violations through the
  normalized observability model.

### v2.1.0 - Async requests and event loops

- Implement one request engine shared by synchronous and asynchronous calls.
- Add async send/receive, cancellation, completion events, request IDs, and
  request cookies.
- Add `zipc_get_fd()`/`zipc_process()` and Linux poll/epoll integration.
- Add explicit inline, deferred, and user-executor callback policies.
- Add explicit FreeRTOS dispatcher tasks; kernel dispatch waits for v5.
- Preserve publication/acknowledgement state so cancellation never returns
  ambiguous ownership silently.

### v2.2.0 - QoS phase 2

- Add multiple priority rings, reserved slots, weighted scheduling, admission
  control, and detailed violation metrics.
- Validate async backpressure, cancellation, fairness, starvation, and deadline
  behavior under saturation.

## v3.x - Broad hardening and security

Essential sanitizer, correctness, and input-validation work remains a v1 gate.
v3 expands qualification across delivered platforms and trust boundaries.

### v3.0.0 - Broad production hardening

- Fuzz configuration, topology, handles, descriptors, trace input, and shared
  metadata.
- Expand race, wraparound, saturation, exhaustion, reconnect, and malformed-input
  campaigns.
- Run multi-day soak and fault-injection tests.
- Add ABI compatibility/layout suites and performance regression thresholds.
- Expand reproducible builds, package generation, Yocto/PetaLinux integration,
  CMake/pkg-config consumption, and coding-standard checks for supported
  deployments.

### v3.1.0 - Security foundation

- Publish a threat model and trust-boundary analysis.
- Validate all untrusted shared and transported metadata.
- Add per-component/per-link authorization and payload-type policy.
- Add integrity/authentication policy, replay-resistant epochs, key lifecycle,
  negative tests, and an independent security review.

### v3.2.0 - Secure platform integration

- Qualify production SMC and FF-A.
- Implement secure memory share/lend/reclaim lifecycle.
- Separate secure/non-secure pool policy and validate secure restart behavior.
- Integrate XMPU/XPPU policy and prepare secure DMA constraints for v5.

### v3.3.0 - Xen and isolation expansion

- Qualify Xen static shared memory and event channels.
- Harden guest restart, event-channel lifecycle, cross-domain authorization, and
  replay protection.

## v4.x - Network production

Network operation uses a versioned wire protocol and serialized payload bytes;
it does not claim cross-host shared-memory zero-copy.

### v4.0.0 - Network wire protocol and TCP

- Define versioned framing, explicit endianness, feature negotiation, message,
  correlation, and flow identity.
- Handle partial I/O, bounded buffering, backpressure, peer identity, reconnect,
  and clear publication/delivery semantics.
- Add two-host interoperability, malformed-frame, disconnect, and soak tests.
- Extend Prometheus, eBPF, Perfetto, and Wireshark support across hosts.
- Add optional OpenTelemetry/OTLP distributed tracing outside the core execution
  path; do not add hidden exporter threads to zIPC.

### v4.1.0 - TCP/TLS and mutual TLS

- Add TLS/mTLS, certificate validation, authorization mapping, key rotation,
  secure reconnect, and actionable TLS diagnostics.

### v4.2.0 - Unreliable UDP datagrams

- Expose loss, duplication, reordering, MTU limits, and bounded datagram
  semantics explicitly.
- Add correlation without implying acknowledgement, ordering, or reliability.

### v4.3.0 - Reliable UDP profile

- Add acknowledgements, retries, ordering, fragmentation/reassembly, duplicate
  suppression, congestion/backpressure behavior, delivery deadlines, and
  session/restart recovery as a distinct profile.

### v4.4.x - Network expansion

- Qualify QUIC, multicast, discovery, RDMA, cross-host zero-copy experiments,
  and multi-hop network routing as separate profiles.

## v5.x - Kernel, DMA, and hardware production

### v5.0.0 - Linux kernel production

- Add Kbuild integration and a production reserved-memory driver.
- Qualify ring/waitqueue and kfifo paths.
- Provide poll/ioctl and/or an in-kernel service API plus workqueue dispatch.
- Integrate lifecycle, recovery, QoS, observability, and access controls.
- Run KUnit, KASAN, KCSAN, lockdep, load/unload, suspend/reset, and stress tests.

### v5.1.0 - DMA and cache coherency

- Add `zipc_buffer_sync_for_cpu()` and `zipc_buffer_sync_for_device()`.
- Integrate Linux DMA APIs and R5 cache-maintenance adapters.
- Define coherent, I/O-coherent, software-maintained, and uncached policies.
- Define valid physical-address and IOVA access.
- Qualify DMA timeout, cancellation, error, and reset behavior.

### v5.2.0 - A53/R5/PL hardware production

- Qualify PL ring/IRQ and DMA completion paths.
- Publish an A53/R5/PL reference deployment.
- Test IRQ loss/storms, cache transitions, reset recovery, data integrity,
  throughput, and tail latency on real hardware.

### v5.3.0 - Advanced buffer model

- Add scatter-gather segments, multi-slot messages, chained allocation/release,
  and header/payload split helpers.
- Add external dma-buf import/export and buffer capability metadata for physical
  contiguity, DMA access, cacheability, security domain, read-only intent, and
  device memory.
- Add segment-aware address and cache-synchronization helpers plus generalized
  BRAM/DDR layouts.

### v5.4.x - Kernel and hardware expansion

- Qualify kernel RPMsg, IPI, PL IRQ, SMC, FF-A, additional SoCs, additional DMA
  engines, and additional hardware security enforcement.

## Postponed feature registry

The following features are retained even when they are not required by the
initial profile:

| Feature | Planned destination |
| --- | --- |
| Public stable link-ID type and local application cookie | v2.x |
| Message, request, correlation, and flow IDs | v2.x/v4.x |
| Payload/access metadata and protocol feature negotiation | v2.x/v4.x |
| Integrity and authentication | v3.x/v4.x |
| Bounded waits and explicit shutdown for the supported Linux profile | v0.9 |
| Transport-independent native per-call durations and uniform try/timed operations | v2.0-v2.1 |
| 32-bit userspace, mixed-bitness, and related ABI qualification | v3.x or product demand |
| Publication acknowledgement and cancellation semantics | v2.x |
| Async engine, callbacks, reactors, and user executors | v2.1 |
| Advanced QoS scheduling and admission control | v2.2 |
| CSV, CTF, and additional trace exporters | CSV v0.5; CTF/additional v3.x or product demand |
| ipctrace compatibility | v0.5/v3.x |
| Configurable or externally supplied trace buffers | v3.x/v5.x |
| Correlation-ID and link-ID trace filtering | v0.5/v2.x |
| OpenTelemetry distributed tracing | v4.0 |
| Expanded `zipc-stat`, `zipc-ping`, `zipc-membench`, and `zipc-packetrate` diagnostics | v0.5/v3.x |
| Production mqueue, FIFO, and Unix datagram qualification | v1.x/v3.x |
| Task-notification, IPI, PL IRQ, SMC, and FF-A qualification | v1.x/v3.x/v5.x |
| R5-to-R5 and bare-metal/OpenAMP RPMsg | v1.2.x |
| Persistent recovery journals and endpoint leases | v3.x or product demand |
| Repeated crash recovery while a replacement is `RECOVERING` | v3.0 |
| Declarative graph/topology and readiness/liveness APIs | v2.x |
| Static platform-configuration generation | v2.x/v5.x |
| Expanded fault-injection and topology-inspection tools | v0.6/v3.x |
| Xen production support | v3.3 |
| UDP, reliable UDP, QUIC, multicast, discovery, and RDMA | v4.x |
| Kernel production support | v5.0 |
| Cache maintenance, DMA, PL, and hardware qualification | v5.1-v5.2 |
| Scatter-gather, multi-slot, dma-buf, and advanced buffers | v5.3 |

Moving an item requires updating its destination and recording the reason in
`docs/DECISIONS.md`. Removing an item also requires an explicit decision there.

## Completed foundation

### v0.1.x

- Fixed-slot ownership, generation-protected handles, component epochs,
  heartbeats, orphan recovery, hop limits, deadlines, timeout-oriented entry
  points, per-slot traces, `zipc-stat`, host-buildable target adapters, topology
  sources, guard pages, strict ownership, version APIs, and ABI-preserving
  geometry/atomic hardening.

### v0.2.0

- Immutable buffer and parent identity, allocator/session/sequence generation,
  secure entropy contracts, exact visited set, buffer-aware traces, publication-
  aware transport results, and abandoned `CLAIMING` recovery.

### v0.3.0

- Pool ABI 3 component lifecycle, caller-driven restart begin/next/finish,
  stable buffer adoption, epoch fencing, Linux SHM ring reconciliation,
  duplicate-send suppression, and seeded five-process crash/restart validation.

No v0.2 or v0.3 target-hardware validation was performed.

## Maintenance

- `TODO.md` identifies the active milestone, tasks, blockers, exit criteria, and
  recently completed work.
- Every feature pull request updates `TODO.md` when active scope changes.
- Every priority, destination, or release-scope change updates this roadmap.
- Every release updates `TODO.md`, this file, `VERSION`, `CHANGELOG.md`,
  `README.md`, and `MANIFEST.txt`.
- A task is complete only when implementation, tests, documentation, and
  validation evidence are complete.
- Backend terminology remains payload backend, descriptor backend, and event
  backend as defined in `docs/BACKENDS.md`.
