# zIPC architecture

## Purpose

zIPC transfers ownership of fixed shared-memory buffers through a chain of
components without copying payload data between components. Components exchange
small handles over per-link transports while payload data remains in shared
memory.

## Three-backend architecture

zIPC requires three logical backend roles to operate:

1. **Payload backend** — stores or transports payload bytes.
2. **Descriptor backend** — transfers `zipc_message_t` descriptors/handles.
3. **Event backend** — wakes or notifies the peer.

A platform may bundle descriptor and event roles into one adapter, but the
roles are distinct. See [`BACKENDS.md`](BACKENDS.md) for the canonical mapping.


## Core objects

### Pool

A pool contains fixed-size slots. It may use:

- one memory region for control and payload; or
- atomic-capable control memory plus separate payload memory.

The pool owns allocation metadata, counters, component lifecycle records, and
slot control structures.

### Slot

A slot contains control metadata and one payload region. The authoritative slot
state is atomic. Other metadata is written only by the current owner and becomes
visible through release/acquire state transitions.

Relevant metadata includes:

- generation;
- owner and next-owner component IDs;
- owner epoch;
- hop count and hop limit;
- visited-component mask;
- payload offset and length;
- transfer sequence;
- deadline;
- error flags;
- fixed-depth trace entries.

### Handle

`zipc_handle_t` is 64 bits:

```text
bits 31..0   slot ID
bits 63..32  generation
```

The generation changes when a slot is reused, allowing stale handle rejection.

### Component

A component has an ID and a runtime epoch. Re-registering the same component ID
creates a newer epoch. Slots retain the owner epoch so orphan recovery can
reclaim data from a dead instance without reclaiming data owned by its restart.

### Link

A link is directional and binds:

- a pool;
- a local component;
- a remote component;
- one transport instance.

Bidirectional communication normally uses two logical links. Future v0.2 work
adds a stable link ID and a local opaque cookie.

## Ownership state model

Typical flow:

```text
FREE -> OWNED -> TRANSFER -> OWNED -> ... -> FREE
```

- Allocation atomically claims `FREE -> OWNED`.
- The current owner prepares payload and transfer metadata.
- Send publishes ownership toward the next owner.
- Receive validates generation, transfer sequence, expected owner, limits, and
  deadline before claiming the slot.
- The final owner releases the slot to `FREE`.
- Recovery may reclaim a slot belonging to a dead component epoch.

Exactly one component owns the buffer at every point. Loops are allowed, so
`hop_count` counts processing passes while `visited_mask` counts distinct
components.

## Payload backends

Current public backends:

- `ZIPC_SHM_POSIX`
- `ZIPC_SHM_HUGEPAGES`
- `ZIPC_SHM_DTREVMEM_CACHED`
- `ZIPC_SHM_DTREVMEM_UNCACHED`
- `ZIPC_SHM_XEN_STATIC`

Control-memory requirements:

- CPU readable;
- CPU writable;
- 32-bit atomic operations;
- correct sharing and ordering attributes for all participants.

Payload memory may reside in DDR, huge pages, Xen static memory, or PL BRAM.
PL BRAM is payload-only unless atomic semantics are explicitly guaranteed.

## Descriptor and event backends

A descriptor backend moves `zipc_message_t` handles and metadata. An event backend wakes the peer. Different hops may use different combinations.

Current concepts include:

- POSIX mqueue;
- Unix datagram;
- FIFO;
- shared ring + eventfd;
- RPMsg/OpenAMP;
- FreeRTOS queue or task notification;
- ZynqMP IPI mailbox;
- PL ring + IRQ;
- Xen ring + event channel;
- SMC and FF-A callback adapters.

A logical link owns its transport instance. Shared rings are SPSC unless a
specific implementation states otherwise.

## Resilience in v0.1

- Component epochs detect restarts.
- Heartbeats provide lifecycle evidence.
- Hop limits and deadlines bound loops and stalls.
- Orphan recovery reclaims slots owned by a failed component epoch.
- Trace entries record allocation, send, receive, release, recovery, and error
  transitions.
- `zipc-stat` inspects pool, component, slot, and trace state.

## Heterogeneous coherency

zIPC state ordering and cache coherency are separate concerns.

- Release/acquire atomics order CPU operations.
- They do not clean dirty cache lines or invalidate stale lines.
- Non-coherent A53/R5/PL paths require platform cache-maintenance hooks.
- Linux production use should rely on a driver and the DMA API where applicable.
- Initial R5 bring-up should use Normal non-cacheable shared memory.

## Future identity model

Planned distinctions:

```text
link_id          stable identity of one logical connection
link_cookie      local application context for a link
request_cookie   local context for one asynchronous operation
correlation_id   end-to-end identity of one message
flow_id          identity of a persistent traffic flow
peer_epoch       identity of one peer runtime instance
```

These values must not be conflated. In particular, pointer-valued cookies are
local to one address space and must never be transported.
