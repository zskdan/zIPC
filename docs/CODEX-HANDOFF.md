# Codex handoff — zIPC v0.1.9

## Objective

Continue development of zIPC from the supplied v0.1.9 repository. This file
captures the design context and implementation expectations that were developed
before the repository was handed to Codex.

## Current release

- Version: `0.1.9`
- Pool ABI: `1`
- Status: experimental prototype; public API and shared-memory ABI are not stable.
- Immediate instruction: inspect and validate v0.1.9 before implementing v0.2.

Recommended first task:

```text
Read AGENTS.md, docs/CODEX-HANDOFF.md, docs/ARCHITECTURE.md,
docs/DECISIONS.md, docs/ROADMAP.md, README.md, and CHANGELOG.md.
Run the complete test suite. Report mismatches between code and documentation.
Do not implement v0.2 until the review is complete.
```

## Completed functionality

### Core protocol

- Fixed shared-memory slots.
- Generation-protected 64-bit handles.
- Exclusive ownership.
- Append, prepend, and trim helpers.
- Loop support through `hop_count` and `visited_mask`.
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
- Control memory must be atomic-capable.
- PL BRAM is payload-only unless atomics are proven.
- Barriers do not replace cache maintenance.
- Same high-level API should work across split-memory and platform backends.
- Every link may use a different transport.
- Loops remain supported and must be bounded by hop limit/deadline.

## Known limitations

- Timeout behavior is not yet uniform across transports.
- FreeRTOS task notification and basic IPI mailbox paths are depth one unless
  paired with a ring or queue.
- Linux cached/uncached `/dev/mem` mappings are prototype behavior.
- No production reserved-memory Linux driver is included.
- Cache-maintenance APIs are planned, not implemented.
- No full async request engine yet.
- Link ID/cookie, correlation ID, QoS model, readiness handshake, and peer state
  APIs are planned, not implemented.
- FreeRTOS and bare-metal integration tests do not replace target hardware runs.

## Agreed future concepts

### Link identity and cookie

Planned v0.2:

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

v0.2 should define, but not fully enforce, a common link QoS model:

- best effort, reliable, low latency, high throughput, real-time classes;
- priority and traffic class;
- queue depth;
- maximum inflight operations;
- latency target and deadline;
- drop policy.

Transport-specific validation/enforcement begins in v0.4 and expands later.

### Async and callbacks

v0.2 defines the API contract and ownership semantics:

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

v0.2 must add:

- complete public API Doxygen comments;
- examples as generated documentation pages;
- architecture/state-machine documentation;
- `Doxyfile`;
- `make docs` generating `build/docs/html/index.html`.

## v0.2 intended scope

The next milestone should focus on protocol identity and semantics, not yet on
advanced buffer chaining or complete asynchronous execution.

Deliverables:

1. stable-in-release link ID and local cookie fields/accessors;
2. correlation ID and optional flow ID;
3. payload type, protocol flags, access intent, priority, traffic class;
4. optional CRC/integrity metadata;
5. required/supported feature masks and negotiation rules;
6. initial QoS structures and validation rules;
7. async public type/API contract with precise ownership documentation;
8. full Doxygen API and example documentation;
9. tests for new metadata, compatibility, and validation;
10. version/changelog/manifest/documentation updates.

Do not silently add full scatter-gather, multi-slot payloads, a worker-thread
async engine, or automated peer readiness into v0.2; those belong to later
roadmap milestones unless explicitly reprioritized.

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
