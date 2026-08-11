# zIPC design decisions

This file records decisions that should not be rediscovered or changed without
an explicit design review.

## D001 — Single-owner buffer protocol

A slot has exactly one owner. zIPC does not currently provide simultaneous
multi-reader ownership or reference-counted fan-out.

## D002 — Atomic state, owner-protected metadata

Only authoritative shared state transitions and shared counters are atomic.
Fields written exclusively by the owner are non-atomic and published through
release/acquire state changes.

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
than once. `visited_mask` represents distinct components and is not a substitute
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

## D012 — Link ID and cookie are different

Planned v0.2 model:

- `link_id`: stable logical link identity, agreed by both sides;
- `cookie`: application-owned local opaque value, never transported.

A pointer cookie is valid only inside its own address space.

## D013 — Message identity is independent from storage identity

Planned `correlation_id` identifies an end-to-end logical message. It is not the
slot handle, generation, transfer sequence, or link ID.

## D014 — QoS is a link contract

QoS configuration belongs to the logical link and may include class, priority,
queue depth, maximum inflight operations, latency target, deadline, and drop
policy. v0.2 defines the model; v0.4 begins enforcement.

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
