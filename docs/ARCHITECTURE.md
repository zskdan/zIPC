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
- `ZIPC_SHM_PREALLOCATED`

Control-memory requirements:

- CPU readable;
- CPU writable;
- 32-bit atomic operations;
- 64-bit atomic operations, because pool ABI 1 actively uses shared
  `_Atomic uint64_t` counters and component lifecycle fields;
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

## Current execution and planned model

In v0.1.11, zIPC calls execute in the caller context and the core creates no
threads. Depending on the selected backend, a send or receive may block or poll
inside that call. There is currently no public asynchronous, service-loop, or
reactor API.

Future releases will preserve caller ownership of execution and make progress
policy explicit. NNG and RPMsg style systems tend to hide per-link/per-socket
worker threads; zIPC rejects that as a future default because hidden threads
cost stack memory, wakeups, context switches, cache pollution, scheduling
jitter, and harder tracing.

The proposed execution modes are:

```text
ZIPC_EXEC_INLINE    synchronous call executes in the caller context
ZIPC_EXEC_POLL      application explicitly calls zipc_poll()/zipc_service()
ZIPC_EXEC_REACTOR   optional, explicitly configured shared reactor thread
```

The planned contract explicitly avoids one thread per link or socket, RX+TX
thread pairs per endpoint, and hidden worker pools. A future async API will not
imply a worker:

```text
zipc_send_async(link, msg, callback, arg)
```

would mean the callback runs when the zIPC execution engine progresses, which
could be `zipc_poll()`, an application event loop, or the optional reactor.

The planned Linux model integrates with the application's own event loop
through a pollable handle and a service entry point:

```c
epoll_wait(epfd, events, n, timeout);
if (zipc_fd_ready(events))
    zipc_process(ctx);   /* progress + callbacks, no zIPC thread */
```

FreeRTOS and bare metal will use the same conceptual model inside an existing
task or superloop; zIPC will not force another RTOS task.

The proposed callback execution policy is explicit so callbacks are never
unexpectedly invoked from arbitrary internal threads:

```text
ZIPC_CALLBACK_INLINE        callback runs in the progress function
ZIPC_CALLBACK_DEFERRED      callback is queued for a later service call
ZIPC_CALLBACK_USER_EXECUTOR callback runs on an application-provided executor
```

Architectural requirement: **zIPC SHALL operate without internally created
threads. An optional shared reactor mode MAY create a bounded number of
explicitly configured worker threads. Thread creation SHALL never scale
implicitly with links, endpoints, or messages.**

A representative acceptance profile:

```text
2 applications, 20 links
    + 2 application threads + 0 zIPC threads        (INLINE/POLL)
    + 2 application threads + 1 reactor per process (REACTOR)
```

## Observability by design

The current v0.1.11 implementation records fixed-depth per-slot entries for
allocation, send, receive, release, recovery, and error transitions. Each
entry contains a timestamp, transfer sequence, component ID, and event type;
`zipc-stat` exposes those entries.

Protocol failures in transfer preparation and claim increment the pool's atomic
`protocol_error_count`. A `ZIPC_TRACE_ERROR` entry is written only when the
caller has already been validated as the current owner. Claim failures before
the ownership state transition deliberately do not write owner-protected slot
trace fields, because doing so could race the legitimate owner/receiver.

Future versions will add an optional fixed-format native trace contract rather
than requiring later reverse engineering. The planned event carries identity
(`link_id`, `message_id`, `correlation_id`, `endpoint_id`, `backend_cookie`),
context (`pid`, `tid`, `timestamp`, `event`, `flags`), and state (`length`,
`queue_depth`). Emission should be opt-in and zero-cost when disabled:

```c
#ifdef ZIPC_TRACE_ENABLE
    ZIPC_TRACE(event);
#else
    #define ZIPC_TRACE(...) do { } while (0)
#endif
```

### Two trace layers

1. **Protocol layer** — planned native zIPC tracepoints:
   `LINK_CREATE`, `LINK_READY`, `LINK_DOWN`; `SEND_BEGIN`, `SEND_QUEUE`,
   `SEND_BACKEND`, `SEND_DONE`; `RECV_BACKEND`, `RECV_QUEUE`, `RECV_DELIVER`,
   `RECV_DONE`; `DROP`, `TIMEOUT`, `RETRY`, `BACKPRESSURE`;
   `CALLBACK_BEGIN`, `CALLBACK_END`.
2. **Backend/kernel layer** — observation of the underlying transport.

Once `message_id` is implemented end-to-end, correlating both layers should be
almost deterministic rather than heuristic. zIPC should require *less*
eBPF/inference over time, not more.

### Tool-neutral trace contract

The planned zIPC trace event model is a single normalized protocol. Tools
(CTF, pcapng for Wireshark, Perfetto) will be exporters from one semantic
model, not four separate formats. Identity is the backbone of the model:

```text
timestamp  source  layer  pid/tid  cpu
link_id  endpoint_id  message_id  correlation_id
event_type  direction  length  queue_depth
transport  transport_id  payload_head  payload_tail  fingerprint
```

Planned backend targets: Linux USDT/tracepoints, FreeRTOS and bare-metal
compact binary rings.

## Reference learning from NNG and RPMsg

NNG and RPMsg are reference systems, not just competitors. Their pain points
define what zIPC must expose natively. While instrumenting them, measure and
document: hidden threads, queuing depth, copies and their location, wakeups,
syscalls per message, context switches per message, message boundary
preservation, backpressure onset, drops, flow control, async/callback
execution context, end-to-end latency decomposition (application, queueing,
transport, scheduler), allocation behavior, endpoint lifecycle
(connect/disconnect/reconnect), and failure behavior (crash, restart,
timeout, malformed data).

These measurements feed the zIPC benchmarking framework, which tracks
version-to-version claims ("zIPC v0.2 reduced application-to-backend latency
by 23%") rather than impressions. The framework measures, per message:
latency p50/p99/max, messages/second, CPU, syscalls, context switches,
wakeups, copies and bytes copied, and per-stage latency. Acceptance testing
adds: threads per process, threads per socket, stack memory, and scheduler
latency.

## Heterogeneous coherency

zIPC state ordering and cache coherency are separate concerns.

- Release/acquire atomics order CPU operations.
- They do not clean dirty cache lines or invalidate stale lines.
- Non-coherent A53/R5/PL paths require platform cache-maintenance hooks.
- Linux production use should rely on a driver and the DMA API where applicable.
- Initial R5 bring-up should use Normal non-cacheable shared memory.

## Planned identity model

The current protocol identifies components, generation-protected slots, and
transfer sequences. Future versions will additionally distinguish identities
that other systems conflate:

```text
link_id          stable identity of one logical connection
link_cookie      local application context for a link
request_cookie   local context for one asynchronous operation
message_id       unique identity of one transmitted message
correlation_id   end-to-end identity of a request/reply or larger transaction
endpoint_id      identity of one endpoint
flow_id          identity of a persistent traffic flow
peer_epoch       identity of one peer runtime instance
backend_cookie   maps zIPC activity onto the underlying backend (NNG, Unix,
                 RPMsg, SHM, ...)
```

These values must not be conflated. In particular, pointer-valued cookies are
local to one address space and must never be transported. A future `message_id`
accompanying each send/receive will make end-to-end correlation deterministic
across protocol and backend layers; `backend_cookie` will let observability map
zIPC activity to the concrete transport without guessing. A stable `link_id`
and local opaque `link_cookie` are v0.2 work.
