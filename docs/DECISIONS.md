# zIPC design decisions

This file records decisions that should not be rediscovered or changed without
an explicit design review.

## D001 — Single-owner buffer protocol

A slot has exactly one owner. zIPC does not currently provide simultaneous
multi-reader ownership or reference-counted fan-out.

## D002 — Atomic state, owner-protected metadata

Authoritative shared state transitions, stale-handle generation, and shared
counters are atomic.
Fields written exclusively by the owner are non-atomic and published through
release/acquire state changes. ABI 2 allocation uses `FREE -> CLAIMING`, and
receive uses `TRANSFER -> CLAIMING`; both complete metadata and local-view
construction before publishing `OWNED` with release. Recovery ignores
`CLAIMING`, and failed claim construction frees directly without pretending an
`OWNED -> FREE` release occurred. Send similarly acquires
`OWNED -> CLAIMING`, validates and writes transfer metadata while exclusive,
revokes strict payload access, and publishes `TRANSFER` only after tracing.

Abandoned `CLAIMING` slots use a separate recovery API. The caller must first
externally quiesce every pool protocol operation because a crash can occur
immediately after the state CAS, before timestamp or claimant metadata exists.
Under full quiescence all remaining claims are abandoned. Recovery ignores
`acquired_ns` and partial component metadata, attributes no component counter,
and restores `CLAIMING` for retry if payload protection fails.

## D003 — Allocation cursor is advisory

The allocation cursor only chooses the first slot to scan. It is a relaxed
atomic to avoid a C data race, but it does not allocate a slot. The slot-state
compare/exchange is authoritative.

## D004 — No global mutex for normal pool operations

Fixed-slot allocation, transfer, receive, release, and recovery use atomic state
transitions. Pool formatting/reset/shutdown remain externally serialized.

## D005 — Split control and payload memory

Control metadata and payload may use separate regions. Control memory must be
atomic-capable. Payload memory may be PL BRAM or another non-atomic region.

## D006 — Loops remain supported

`hop_count` is retained because a component may process the same buffer more
than once. The exact visited set represents distinct components and is not a substitute
for `hop_count`.

## D007 — Generation-protected handles

Handles contain slot ID and generation. Generation is checked on receive and
release to detect stale descriptors after slot reuse.

## D008 — Transport per logical link

Each connection may select a different transport. For example, A↔B may use a
queue while B↔C uses a Unix socket, RPMsg, or IPI.

## D009 — Reserved DDR for heterogeneous metadata

A53/R5/PL control metadata belongs in reserved DDR with suitable memory
attributes. PL BRAM is payload-only unless atomic support is proven.

## D010 — Cache maintenance is explicit platform behavior

Memory barriers do not perform cache maintenance. Future v0.5 APIs will expose
sync-for-CPU/device operations. Until then, integrations must provide correct
mapping or cache operations externally.

## D011 — Component epoch identifies an instance

Component ID identifies a logical component. Epoch identifies one runtime
instance. Recovery must match both component ID and epoch.

Owner and abandoned-claim recovery are administrative operations and require
pool-wide protocol quiescence. Recovery must not probe live transfers while a
receiver can consume their only descriptor notification.

## D012 — Link ID and cookie are different

Deferred post-v0.2.0 model:

- `link_id`: stable logical link identity, agreed by both sides;
- `cookie`: application-owned local opaque value, never transported.

A pointer cookie is valid only inside its own address space.

## D013 — Message identity is independent from storage identity

Pool ABI 2 `buffer_id` identifies one allocated buffer storage lifetime and
`parent_id` records direct creation lineage. These are not a request/reply
correlation ID, slot handle, generation, transfer sequence, or link ID.

## D014 — QoS is a link contract

QoS configuration belongs to the logical link and may include class, priority,
queue depth, maximum inflight operations, latency target, deadline, and drop
policy. This model is deferred beyond v0.2.0; v0.4 begins enforcement.

## D015 — Preserve synchronous and asynchronous APIs

Planned async APIs do not replace `zipc_send()` and `zipc_receive()`. Long-term,
synchronous calls should wrap the same internal request engine used by async
operations.

## D016 — Callback execution is deferred by default

Normal callbacks should not execute in Linux hard IRQ or FreeRTOS ISR context.
Platform adapters should queue completion and dispatch from a worker, task,
workqueue, caller event loop, or explicit polling context.

## D017 — Ready, alive, connected, and healthy are distinct

Future lifecycle support must distinguish:

- connected: transport endpoint exists;
- ready: protocol, memory, and link handshake completed;
- alive: recent heartbeat or activity exists;
- healthy: alive without relevant protocol/resource failures.

## D018 — Doxygen becomes mandatory in v0.2

v0.2 adds complete public API and example documentation, a `Doxyfile`, and
`make docs`. Every new public API from that point must include Doxygen comments.

## D019 — Host stubs are not hardware validation

FreeRTOS and bare-metal host integration tests compile and exercise real target
adapter source with stubs. They do not validate BSP cache attributes, OpenAMP,
IPI hardware, interrupt latency, or the target memory map.


## Backend terminology

Use the canonical three-role model: payload backend, descriptor backend, and event backend. See `docs/BACKENDS.md`.

## D020 — Descriptor publication determines send ownership

`ZIPC_ERR_TRANSPORT` is reserved for failures known to occur before descriptor
visibility, and permits `TRANSFER -> OWNED` sender rollback.
`ZIPC_ERR_TRANSPORT_PUBLISHED` reports an error after descriptor publication;
the core returns the error but consumes sender ownership and leaves the slot in
`TRANSFER`. Ring/mailbox plus event paths use this status when the event fails.
An integrated operation whose callback cannot distinguish its failure point is
classified conservatively as published once publication may have occurred.

## Buffer ownership protection

- The normal zIPC data path uses protocol/API ownership enforcement and does
  not change virtual-memory permissions on every ownership transfer.
- `zipc_send()` is expected to consume/invalidate the sender's local buffer
  object on successful ownership transfer; accessors must reject invalid
  buffer objects in the simplified public API.
- Slot generation detects stale handles after slot reuse.
- Guard pages are an optional pool property used to catch linear
  overrun/underrun at slot boundaries; they do not enforce ownership between
  otherwise valid slots.
- Per-transfer MMU/MPU ownership enforcement is reserved for an optional
  strict/debug protection mode. On Linux this may use per-slot `mprotect()`
  (`RW` for the current owner and `PROT_NONE` for non-owners).
- Strict protection must remain optional because page-permission and TLB
  updates can materially increase transfer latency.
- The strict ownership protection mode is a planned capability, not an
  implemented v0.1.6 guarantee.


## v0.1.7 application API and ownership

- `zipc_buffer_t` is opaque; public code uses accessors only.
- `zipc_send()` and `zipc_buffer_release()` consume and invalidate the local buffer object on success.
- Pools are independent topology objects; links reference a pool but do not own pool lifetime.
- A descriptor carries `pool_id` in addition to the slot handle.
- Named link opening is backed by a process-local static topology registry; explicit `zipc_link_create()` remains the expert API.
- Guard pages detect linear slot overrun. Strict ownership is a separate optional Linux protection mode that revokes non-owned slot mappings with `mprotect()`.
- Copy helpers exist specifically to reduce first-stage NNG migration friction; zero-copy buffer ownership remains the preferred high-performance model.

## D021 — Buffer identity layout and lifetime

- `zipc_buffer_id_t` is packed numerically as allocator:8, session:24,
  sequence:32; C bitfields are prohibited.
- Full zero means no parent/root. Usable allocator IDs are 1..254.
- Identity and parent are immutable slot-control fields for one allocation
  lifetime. Descriptors continue to reference the authoritative slot.
- All links with one local component ID share one process-global generator.
- The generator uses only 32-bit atomics and secure platform entropy. It does
  not automatically reseed across `fork()`.
- Issuers join an active count and recheck the stable session before reserving a
  sequence. A rotator first blocks entrants, then waits for active issuers to
  drain before changing session and resetting sequence.

## D022 — Exact visited set

The 256-entry component namespace uses eight owner-protected non-atomic
`uint32_t` words. Reserved bits 0 and 255 are never set. Topology link capacity
remains independently fixed at 64.

## D023 — ABI 2 atomic baseline

Identity generation itself requires only 32-bit atomics. Pool ABI 2 retains the
ABI-1 shared `_Atomic uint64_t` counters and lifecycle timestamps because
replacing them cleanly is outside the identity-only milestone. Control memory
therefore still requires both atomic32 and atomic64; correctness is not weakened.

## D024 — Native trace seam

The fixed trace hook is optional, synchronous, non-reentrant, allocation-free in
core, and runs in caller context. It must not call zIPC or block protocol
progress; hook configuration is externally serialized with protocol calls.
Slot history is written only by a validated owner.
Owner-unsafe errors may emit an external record with zero identity and invalid
handle without touching slot metadata.

## D025 — Online restart recovery is epoch-fenced and transport-specific

Pool ABI 3 stores component epoch and lifecycle state in one atomic packed value
with `INACTIVE`, `RECOVERING`, and `ACTIVE` states. A replacement runtime may
start online recovery only after the exact old runtime is known to have
terminated. `zipc_component_restart_begin()` advances to a new `RECOVERING`
epoch; old high-level buffers and links are rejected by epoch fences.
This partially supersedes D012: a stable link-ID configuration field is now
implemented for recovery, while the local application cookie remains deferred.

Stable old-epoch `OWNED` buffers are adopted in place. Their immutable
`buffer_id`, parent lineage, and payload are preserved, while owner epoch and
handle generation advance. The application handler reruns from its beginning.
External side effects are consequently at-least-once and applications should
deduplicate by `buffer_id`; zIPC does not claim a persistent per-cell journal or
endpoint leases.

Online `zipc_link_reconcile()` is supported only for Linux SHM ring eventfd and
polling transports, where peek/claim/commit and inspectable ring publication make
repair possible. Duplicate pending sends are suppressed and eventfd checks the
ring before waiting. Other transports return
`ZIPC_ERR_RECOVERY_UNSUPPORTED` until they define equivalent inspectable and
repairable publication semantics.

The v0.3.0 guarantee applies to registered nonzero epochs and externally
supplied shared-ring mappings. Transfer metadata persists the stable link ID so
reconciliation cannot move a descriptor to another same-peer link. A second
crash during `RECOVERING` is deferred; it requires quiesced administrative
recovery in this release.
