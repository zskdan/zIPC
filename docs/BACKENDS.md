# zIPC backend model

zIPC operates through three independent logical backend roles:

```text
payload backend     stores or carries the buffer bytes
descriptor backend  queues/transfers zipc_message_t handles and metadata
event backend       wakes or notifies the receiving component
```

A link is therefore understood as:

```text
payload + descriptor + event
```

The roles are independent even when a platform adapter bundles two of them.
For example, `ZIPC_TRANSPORT_SHM_RING_EVENTFD` is a compound adapter made of a
shared descriptor ring and an eventfd event backend.

## 1. Payload backends

Payload backends answer: **where are the bytes stored or carried?**

| Backend | Typical use |
|---|---|
| `ZIPC_SHM_POSIX` | Linux processes sharing RAM |
| `ZIPC_SHM_HUGEPAGES` | Large Linux shared pools |
| `ZIPC_SHM_DTREVMEM_CACHED` | Cached reserved DDR |
| `ZIPC_SHM_DTREVMEM_UNCACHED` | Uncached reserved DDR or mapped device memory |
| `ZIPC_SHM_XEN_STATIC` | Xen static shared memory |
| `ZIPC_SHM_PREALLOCATED` | Global array, linker section, OCRAM/TCM, or BSP-owned memory |

Future TCP/UDP payload backends use serialized bytes rather than a remotely
dereferenceable shared-memory handle.

## 2. Descriptor backends

Descriptor backends answer: **how does the peer receive the handle and transfer metadata?**

- message queue;
- Unix datagram;
- FIFO;
- shared SPSC ring;
- RPMsg;
- shared mailbox;
- Xen ring;
- PL ring;
- synchronous secure-call request/response.

## 3. Event backends

Event backends answer: **how does the peer know that descriptors are available?**

- integrated wakeup in a queue/socket/RPMsg implementation;
- eventfd;
- shared-memory polling (busy polling of the descriptor ring);
- FreeRTOS task notification;
- IPI;
- Xen event channel;
- IRQ;
- synchronous secure-call completion.

## Current compound adapters

| Existing transport adapter | Descriptor role | Event role |
|---|---|---|
| POSIX/FreeRTOS message queue | Message queue | Integrated |
| Unix datagram | Unix datagram | Integrated |
| FIFO | FIFO | Integrated |
| SHM ring + eventfd | Shared ring | eventfd |
| SHM ring + polling | Shared ring | shared-memory polling |
| RPMsg | RPMsg | Integrated |
| Task notification | Shared mailbox | Task notification |
| IPI | Shared mailbox | IPI |
| Xen ring + event channel | Xen ring | Xen event channel |
| PL ring + IRQ | PL ring | IRQ |
| SMC / FF-A | Secure-call descriptor | Synchronous completion |

The public role enums and `zipc_transport_backend_roles()` make this composition
explicit. Existing compound transport names remain supported in v0.3 for
compatibility.

## Send publication contract

- `ZIPC_ERR_TRANSPORT`: the descriptor was not visible; core rollback is safe.
- `ZIPC_ERR_TRANSPORT_PUBLISHED`: the descriptor was visible before a later
  notification, IRQ, event-channel, or callback error; rollback is unsafe.

For shared ring/event paths, a full ring is an ordinary transport failure while
a failed event after producer-index publication is a published failure. Mailbox
signal callbacks run after mailbox publication and any callback error is
therefore published. Synchronous SMC/FF-A callback errors cannot identify the
visibility point and are conservatively reported as published. The core never
converts a published failure to success: it invalidates sender ownership and
leaves the slot in `TRANSFER` for receiver progress or explicit recovery.

## Linux SHM ring restart behavior

The eventfd and polling SHM ring transports are the only v0.3.0 backends with
online link reconciliation. Receive is split into peek/claim/commit: the ring
entry remains recoverable until the pool claim succeeds, then consumption is
committed. Abort leaves the descriptor pending. The producer suppresses an
already-pending duplicate send during reconciliation.

The eventfd receiver always checks the ring before waiting. Notification is a
wakeup hint rather than the authority for descriptor availability, preventing a
published ring entry from being stranded if its eventfd signal was consumed or
lost across a crash. Polling observes the same ring directly.

`zipc_link_reconcile()` returns `ZIPC_ERR_RECOVERY_UNSUPPORTED` for every other
transport in v0.3.0. No equivalent recovery guarantee is implied for queues,
sockets, FIFO, RPMsg, IPI, PL IRQ, Xen, SMC, or FF-A.

## Example compositions

```text
Linux processes:
    payload    = POSIX SHM
    descriptor = shared SPSC ring
    event      = eventfd or shared-memory polling

FreeRTOS tasks:
    payload    = PREALLOCATED global/linker memory
    descriptor = shared mailbox
    event      = task notification

Linux <-> R5:
    payload    = reserved DDR
    descriptor = RPMsg or shared mailbox/ring
    event      = RPMsg-integrated notification or IPI

PS <-> PL:
    payload    = DDR or BRAM
    descriptor = PL-visible ring
    event      = doorbell + IRQ
```

## Shared-memory polling event backend

`ZIPC_TRANSPORT_SHM_RING_POLLING` combines the shared SPSC descriptor ring with
`ZIPC_EVENT_BACKEND_SHM_POLLING`. The receiver continuously observes the ring
producer index and consumes a descriptor as soon as it appears. No eventfd,
interrupt, syscall, or scheduler wakeup is required on the data path.

Polling is a true busy-spin path using a CPU-relax instruction. `poll_timeout_ns == 0` waits forever; any non-zero value uses `CLOCK_MONOTONIC` and returns `ZIPC_ERR_TIMEOUT` on expiry.

This backend is intended for dedicated cores/threads and latency-critical paths.
It consumes CPU while idle and should not be the default for general-purpose
Linux processes. The shared ring metadata must be mapped as Normal memory with
working acquire/release atomics and coherent visibility between peers.

## Identity Entropy

Buffer identity generation is process-local core policy backed by a platform
entropy operation. Linux userspace and hosted Xen use robust `getrandom()`
loops; Linux kernel uses readiness-aware `get_random_bytes_wait()` and
propagates interruption/failure. FreeRTOS and bare-metal builds
must define `ZIPC_FREERTOS_RANDOM` or `ZIPC_BAREMETAL_RANDOM` BSP functions with
the signature `zipc_status_t fn(void *, size_t)`. Missing hooks fail explicitly;
there is no time or `rand()` fallback.
