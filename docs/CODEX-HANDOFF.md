# Codex handoff — zIPC v0.3.0

## Objective

Continue from the v0.3.0 supervisorless relay restart recovery milestone toward
v0.4.0 core observability/debuggability and the first delivered Linux userspace
product at v1.0.0.

## Current release

- Version: `0.3.0`
- Pool ABI: `3`
- Status: experimental prototype; public API and shared-memory ABI are not stable.
- Scope: online recovery of a terminated relay runtime on Linux SHM ring
  eventfd/polling links, building on v0.2 buffer identity and lineage.
- Active checklist: `TODO.md`.
- Canonical delivery plan: `docs/ROADMAP.md`.
- Production status: not delivered or deployed; all v0.x work prepares Linux
  userspace readiness.

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

### v0.3 restart recovery

- One atomic packed component lifecycle value combines epoch with `INACTIVE`,
  `RECOVERING`, or `ACTIVE` state.
- `zipc_component_restart_begin()` fences the dead epoch and creates the next
  epoch in `RECOVERING`; the caller must have terminated the exact old runtime.
- `zipc_link_reconcile()` repairs interrupted transport state for Linux SHM
  ring eventfd and polling links only.
- `zipc_component_restart_next()` adopts stable old-epoch `OWNED` buffers while
  preserving buffer identity, lineage, and payload and bumping handle generation.
- `zipc_component_restart_finish()` publishes `ACTIVE` only after reconciliation
  and adoption are complete.
- Old high-level buffers and links are epoch-fenced. Adopted buffers rerun their
  application handler from the start, so external side effects are at-least-once
  and should be deduplicated by `buffer_id`.
- Linux ring receive uses peek/claim/commit; duplicate pending sends are
  suppressed; eventfd receive checks the ring before waiting.
- Online recovery requires registered nonzero epochs, explicit link roles and
  IDs, and externally supplied shared-ring mappings. A second crash while the
  replacement remains `RECOVERING` uses quiesced administrative recovery.

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
- Linux five-process randomized relay restart recovery.
- Five-process Linux ready-to-play test.
- Host-buildable FreeRTOS adapter integration.
- Host-buildable bare-metal adapter integration.

## Important design constraints

- Only the slot state and shared counters are necessarily atomic.
- Exclusive ownership protects ordinary slot fields.
- The allocation cursor is a hint, not the allocation operation.
- No pool-wide lock is required for normal fixed-slot operations.
- Pool format/reset/shutdown requires external serialization.
- Pool ABI 3 control memory must support CPU read/write plus 32-bit and 64-bit
  atomics.
- PL BRAM is payload-only unless atomics are proven.
- Barriers do not replace cache maintenance.
- Same high-level API should work across split-memory and platform backends.
- Every link may use a different transport.
- Loops remain supported and must be bounded by hop limit/deadline.

## Known limitations

- Timeout behavior is not uniform across transports. ABI 3 compatibility
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
- General link identity APIs/local cookie, correlation ID, QoS model, readiness
  handshake, and peer state APIs are planned, not implemented.
- FreeRTOS and bare-metal integration tests do not replace target hardware runs.
- Online link reconciliation supports only Linux
  `ZIPC_TRANSPORT_SHM_RING_EVENTFD` and `ZIPC_TRANSPORT_SHM_RING_POLLING`;
  all other transports return `ZIPC_ERR_RECOVERY_UNSUPPORTED`.
- Recovery has no persistent per-cell journal or endpoint leases.
- Recovery cannot make external side effects exactly-once; applications must
  deduplicate replay by immutable `buffer_id` where required.

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

Pool ABI 3 link configuration includes the stable nonzero `uint64_t link_id`
needed for supported restart reconciliation. A dedicated public ID type and
application cookie remain deferred:

```c
typedef uint64_t zipc_link_id_t;
typedef uintptr_t zipc_cookie_t;
```

- Link ID is stable and identifies the logical connection; its configuration
  field is implemented for v0.3 recovery.
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

Transport-specific validation/enforcement begins in v2.0 and expands later.

### Async and callbacks

Post-v0.2.0 work defines the API contract and ownership semantics:

- request IDs;
- request cookies;
- async send/receive;
- cancellation;
- completion events;
- delivery/publication state;
- callback execution context.

v2.1 implements the request engine, deferred completion dispatch, native
timeouts, cancellation, and Linux poll/epoll integration.

Key ownership rule: after successful async send submission, the application
must not touch the buffer. A timeout after publication is ambiguous and cannot
blindly return ownership without recovery/acknowledgement semantics.

### Peer readiness and liveness

The v2.x execution/QoS work makes lifecycle and compatibility negotiation
first-class:

- link state: down, starting, ready, degraded, failed;
- peer state: unknown, booting, ready, alive, stopping, dead, restarted;
- readiness handshake;
- heartbeat/liveness detection;
- peer epoch/restart detection;
- link lifecycle callbacks.

Readiness should validate link ID, component identity, epoch, ABI/features,
transport setup, memory-layout agreement, cache policy, and QoS compatibility.

### Doxygen

`Doxyfile` and `make docs` exist. New v0.3.0 public APIs are documented; v0.8.0
completes the remaining public API audit and makes missing documentation fail
CI.

## v0.3.0 recovery scope

- Recovery is online with respect to other live component runtimes and does not
  require a pool-wide supervisor or pool-wide quiescence.
- The exact old relay runtime must be terminated before restart begins. An epoch
  fence is not permission to let old code continue running.
- Component lifecycle epoch/state changes atomically, preventing readers from
  observing a mixed epoch and state.
- Stable old-epoch `OWNED` buffers are adopted in place. `buffer_id`, parent ID,
  and payload remain unchanged; handle generation and owner epoch change.
- Link reconciliation is deliberately restricted to the two Linux SHM ring
  transports whose descriptor publication can be inspected and repaired.
- Application handling of an adopted buffer restarts from its beginning. zIPC
  does not persist application program counters or side-effect journals.
- `tests/recovery-chain-linux.c` runs A->B->C->D->E, randomly kills one of three
  relays at seeded checkpoints, defaults to 10 iterations for each eventfd and
  polling backend, and reports protocol/service min/mean/p50/p95/max recovery.
  Normal output names each scenario, `--verbose` adds a timestamped stage and
  latency breakdown, and `--quiet` limits successful runs to aggregate output.
  Protocol failures quiesce surviving children before dumping their collected
  timeline, component lifecycles, ring occupancy, active slots, and replay
  command.
- Release validation is Linux-host only; no target hardware was validated.

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

QoS, async, CRC/integrity, flow/request IDs, public link identity, readiness,
and advanced buffer models remain scheduled in the maintained roadmap rather
than an open-ended v0.2.1+ bucket.

## Release discipline

For each version:

- update `VERSION`;
- update `CHANGELOG.md`;
- update `README.md`, `TODO.md`, and roadmap status;
- update `MANIFEST.txt`;
- add tests for each public behavior;
- run `make clean && make test && make utilities`;
- document whether hardware tests were executed;
- avoid direct commits to `master`;
- produce a reviewable pull request.


## Backend terminology

Use the canonical three-role model: payload backend, descriptor backend, and event backend. See `docs/BACKENDS.md`.
