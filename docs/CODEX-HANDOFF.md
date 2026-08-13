# Codex handoff — zIPC v0.2.0

## Objective

Continue development from the focused v0.2.0 buffer-identity milestone.

## Current release

- Version: `0.2.0`
- Pool ABI: `2`
- Status: experimental prototype; public API and shared-memory ABI are not stable.
- Scope: buffer identity/correlation lineage plus required namespace, visited
  set, entropy, ABI, and trace support.

Recommended first task: read the authoritative repository documents, run the
complete test suite, and report code/documentation mismatches before extending
the protocol.

## Completed functionality

### Core protocol

- Fixed shared-memory slots.
- Transient `CLAIMING` publication state for fully initialized ownership views.
- Generation-protected 64-bit handles.
- Exclusive ownership.
- Append, prepend, and trim helpers.
- Loop support through `hop_count` and the exact visited set.
- Per-transfer sequence tracking.
- Separate control and payload memory.
- Relaxed atomic allocation cursor plus atomic slot claim.

### v0.1 resilience

- Component registration and epochs.
- Heartbeats and unregister support.
- Orphan recovery by component ID and epoch.
- Per-buffer hop limits and absolute deadlines.
- Timeout-oriented send/receive entry points.
- Fixed-depth trace entries.
- `zipc-stat` diagnostics.

### Platforms and transports

- Linux userspace.
- Linux kernel adapter skeleton/helpers.
- FreeRTOS.
- Bare metal/OpenAMP-oriented adapter.
- Xen static shared memory and event-channel concepts.
- POSIX mqueue, Unix datagram, FIFO, ring/eventfd, RPMsg, task notification,
  IPI, PL ring/IRQ, Xen ring/event channel, SMC, and FF-A abstractions.

### Utilities

- `zipc-ping`
- `zipc-membench`
- `zipc-packetrate`
- `zipc-stat`

### Examples

- Minimal Linux example.
- Five Linux processes.
- Five FreeRTOS tasks.
- Linux → FreeRTOS R5_0 → bare-metal R5_1 → Linux chain blueprint.
- Universal heterogeneous topology blueprint.

### Tests

- Linux integration.
- Linux resilience.
- Five-process Linux ready-to-play test.
- Host-buildable FreeRTOS adapter integration.
- Host-buildable bare-metal adapter integration.

## Important design constraints

- Only the slot state and shared counters are necessarily atomic.
- Exclusive ownership protects ordinary slot fields.
- The allocation cursor is a hint, not the allocation operation.
- No pool-wide lock is required for normal fixed-slot operations.
- Pool format/reset/shutdown requires external serialization.
- Pool ABI 2 control memory must support CPU read/write plus 32-bit and 64-bit
  atomics.
- PL BRAM is payload-only unless atomics are proven.
- Barriers do not replace cache maintenance.
- Same high-level API should work across split-memory and platform backends.
- Every link may use a different transport.
- Loops remain supported and must be bounded by hop limit/deadline.

## Known limitations

- Timeout behavior is not uniform across transports. ABI 2 compatibility
  timeout entry points cannot override the timeout fixed when a backend is
  opened; the per-call argument has no portable duration semantics.
- Transport sends distinguish safe pre-publication failure from
  `ZIPC_ERR_TRANSPORT_PUBLISHED`. The latter consumes sender ownership and
  leaves the slot in `TRANSFER` after a notification/event error.
- FreeRTOS task notification and basic IPI mailbox paths are depth one unless
  paired with a ring or queue.
- Linux cached/uncached `/dev/mem` mappings are prototype behavior.
- No production reserved-memory Linux driver is included.
- Cache-maintenance APIs are planned, not implemented.
- No full async request engine yet.
- Link ID/cookie, correlation ID, QoS model, readiness handshake, and peer state
  APIs are planned, not implemented.
- FreeRTOS and bare-metal integration tests do not replace target hardware runs.

## v0.1.11 hardening findings

- Pool ABI 1 actively uses `_Atomic uint64_t`; the previous ATOMIC32-only
  requirement was incorrect and is now rejected/documented.
- Pool format/attach now use checked alignment and size arithmetic, reject
  same-object control/payload overlap, and validate exact header/control offsets
  before deriving pointers.
- Epoch wrap cannot produce zero; exhausted epochs return
  `ZIPC_ERR_COMPONENT_STALE`, and recovery rejects an active exact epoch.
- Relative deadline overflow saturates to `ZIPC_DEADLINE_NONE`.
- Prepare/claim protocol failures increment `protocol_error_count`; slot error
  traces are emitted only after ownership validation to avoid data races.
- Strict receive-protection failure releases the newly claimed slot instead of
  silently orphaning it in `OWNED` state.

## Agreed future concepts

### Link identity and cookie

Deferred beyond v0.2.0:

```c
typedef uint64_t zipc_link_id_t;
typedef uintptr_t zipc_cookie_t;
```

- Link ID is stable and identifies the logical connection.
- Cookie is application-owned local context.
- Cookie must not be transported.
- A pointer cookie is meaningful only in one address space.

### Message identity

- `correlation_id`: one end-to-end message.
- `flow_id`: persistent stream/traffic flow.
- Neither replaces slot generation or transfer sequence.

### QoS

Post-v0.2.0 work should define, but not fully enforce, a common link QoS model:

- best effort, reliable, low latency, high throughput, real-time classes;
- priority and traffic class;
- queue depth;
- maximum inflight operations;
- latency target and deadline;
- drop policy.

Transport-specific validation/enforcement begins in v0.4 and expands later.

### Async and callbacks

Post-v0.2.0 work defines the API contract and ownership semantics:

- request IDs;
- request cookies;
- async send/receive;
- cancellation;
- completion events;
- delivery/publication state;
- callback execution context.

v0.4 implements the request engine, deferred completion dispatch, native
timeouts, cancellation, and Linux poll/epoll integration.

Key ownership rule: after successful async send submission, the application
must not touch the buffer. A timeout after publication is ambiguous and cannot
blindly return ownership without recovery/acknowledgement semantics.

### Peer readiness and liveness

v0.6 makes lifecycle first-class:

- link state: down, starting, ready, degraded, failed;
- peer state: unknown, booting, ready, alive, stopping, dead, restarted;
- readiness handshake;
- heartbeat/liveness detection;
- peer epoch/restart detection;
- link lifecycle callbacks.

Readiness should validate link ID, component identity, epoch, ABI/features,
transport setup, memory-layout agreement, cache policy, and QoS compatibility.

### Doxygen

`Doxyfile` and `make docs` exist. New v0.2.0 public APIs are documented; a
complete audit of unrelated pre-existing APIs and generated example pages is
deferred.

## v0.2.0 identity scope

- Buffer IDs pack allocator component, random runtime session, and sequence.
- Parent ID records direct immutable allocation lineage; zero denotes a root.
- One process-global generator table is shared by links with the same component.
- Sequence zero through `UINT32_MAX - 1` are used; the max value rotates session
  after a 32-bit atomic active-issuer gate drains.
- Fork is not automatically detected or reseeded.
- ABI 2 uses a 256-entry component table and exact eight-word visited set.
- Existing shared atomic64 counters remain, so control memory still requires
  atomic32 and atomic64. The local generator only requires atomic32.
- Fixed trace records cover allocate/send/receive/release/recover/error.
- Abandoned `CLAIMING` recovery requires full pool quiescence, reclaims every
  remaining claim without age/component attribution, and is retryable after
  protection failure.
- No target hardware validation was performed for v0.2.0; target adapter runs
  use Linux-host stubs/simulation only.

QoS, async, CRC, flow/request IDs, link identity, readiness, and advanced
buffer models are deferred to v0.2.1+ or later roadmap milestones.

## Release discipline

For each version:

- update `VERSION`;
- update `CHANGELOG.md`;
- update `README.md` and roadmap status;
- update `MANIFEST.txt`;
- add tests for each public behavior;
- run `make clean && make test && make utilities`;
- document whether hardware tests were executed;
- avoid direct commits to `main`;
- produce a reviewable pull request.


## Backend terminology

Use the canonical three-role model: payload backend, descriptor backend, and event backend. See `docs/BACKENDS.md`.
