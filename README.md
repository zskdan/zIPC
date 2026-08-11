# zIPC v0.1.19

zIPC is an experimental chained zero-copy IPC protocol. A component allocates
a fixed slot from a shared pool, processes the payload in place, and transfers
only a generation-protected slot handle to the next component. Exactly one
component owns a slot at a time. Loops are supported: `hop_count` records total
processing passes and `visited_mask` records distinct components.

**Status:** resilience-focused prototype. v0.1 is not ABI-stable or production-ready.



## Application API (v0.1.19)

The normal application path is intentionally small:

```c
zipc_link_open(&link, "ab");
zipc_buffer_alloc(link, size, &buffer);
zipc_send(link, &buffer);        /* ownership transfer; buffer invalid afterward */
zipc_recv(link, &buffer);
zipc_buffer_release(&buffer);    /* buffer invalid afterward */
```

`zipc_buffer_t` is opaque and stack-allocatable. For an initial NNG migration,
`zipc_send_copy()` and `zipc_recv_copy()` provide a simpler copy-oriented path.
See [`docs/API.md`](docs/API.md), [`docs/NNG-MIGRATION.md`](docs/NNG-MIGRATION.md),
and [`examples/shared-buffer-chain/`](examples/shared-buffer-chain/).

## What is new in v0.1

- Component epochs, heartbeats, unregister/restart detection, and explicit orphan-slot recovery.
- Per-buffer hop limits and absolute deadlines to bound chains with loops.
- Timeout-oriented `zipc_send_timeout()` and `zipc_receive_timeout()` entry points; the current v0.1 fallback uses backend-configured blocking semantics where a transport has no native timed operation.
- Fixed-depth per-slot trace history for allocation, send, receive, release, and recovery.
- `zipc-stat` for pool counters, component lifecycle state, active slots, ages, and trace entries.
- A release roadmap from v0.1 through v1.0 in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Core model

- Handle: `{generation, slot_id}` to reject stale references.
- Atomic synchronization: only authoritative slot state and pool-wide counters.
- Optional split memory: atomic-capable control memory plus independent payload
  memory, including uncached PL BRAM.
- Payload region: offset and length support prepend and append without copying
  the whole buffer.
- Per-link transport selection: every hop can use a different backend.

## Source layout

```text
include/zipc/zipc.h                 public userspace/RTOS/bare-metal API
include/zipc/zipc-kernel.h          Linux kernel adapter API
src/core/zipc.c                     platform-independent pool protocol
platform/linux/common/              shared Linux ring helpers
platform/linux/user/                POSIX/Linux userspace adapters
platform/linux/kernel/              kernel kfifo/ring/waitqueue adapter
platform/freertos/                  FreeRTOS adapters
platform/baremetal/                 bare-metal adapters
platform/xen/                       Xen static SHM + event-channel adapter
examples/basic/                     minimal runnable Linux example
examples/universal/                 heterogeneous integration blueprint
tests/                              Linux integration regression
```


## High-level API

The low-level pool and platform APIs remain available for integration code.
Normal applications use named directional links and opaque buffers:

```c
zipc_link_open(&producer, "ab");
zipc_buffer_alloc(producer, payload_size, &buffer);
memcpy(zipc_buffer_data(&buffer), payload, payload_size);
zipc_send(producer, &buffer);          /* consumes buffer */

zipc_link_open(&consumer, "ab");
zipc_recv(consumer, &buffer);
zipc_buffer_prepend(&buffer, header, header_size);
zipc_buffer_release(&buffer);          /* consumes buffer */
```

Main application API:

- `zipc_link_open()` / `zipc_link_destroy()`
- `zipc_buffer_alloc()` / `zipc_buffer_alloc_ex()`
- `zipc_send()` / `zipc_recv()`
- `zipc_buffer_release()`
- `zipc_send_copy()` / `zipc_recv_copy()` for first-stage migration
- `zipc_buffer_data()` / `zipc_buffer_size()`
- `zipc_buffer_at()` for checked absolute fixed-offset access
- `zipc_buffer_headroom()` / `zipc_buffer_tailroom()`
- append/prepend/trim helpers

`zipc_link_create()` and the platform/backend objects remain the expert API.
Compatibility wrappers for the older v0.x names remain available.

Fixed-layout components can access checked absolute offsets without exposing
internal slot pointers:

```c
struct comp_b_fields *b =
    zipc_buffer_at(&buffer, 0x40, sizeof(*b));
if (b == NULL)
    return ZIPC_ERR_REGION_OVERFLOW;
```

`zipc_buffer_at()` is relative to buffer offset zero, while
`zipc_buffer_data()` follows the current logical data window. Front/back trims
change only that logical window and do not move bytes.

A link is directional: `local_component` is the owner on receive/allocation and
`remote_component` is the peer on send. Bidirectional communication normally
uses two links or a bidirectional transport configured as two logical links.

## Shared-memory backends

- `ZIPC_SHM_POSIX`
- `ZIPC_SHM_HUGEPAGES`
- `ZIPC_SHM_DTREVMEM_CACHED`
- `ZIPC_SHM_DTREVMEM_UNCACHED`
- `ZIPC_SHM_XEN_STATIC`
- `ZIPC_SHM_PREALLOCATED` — caller-owned static array, linker section, OCRAM/TCM, or BSP-provided memory

`control_memory` must advertise CPU read/write and 32-bit atomic capability.
`payload_memory` may be separate and does not need atomic support.

### Optional Linux guard pages

A payload pool can set `ZIPC_POOL_F_GUARD_PAGES` to insert one `PROT_NONE`
virtual-memory page after every page-aligned POSIX-SHM slot. A linear write past
`slot_capacity` then faults before corrupting the next slot. Use
`zipc_pool_guarded_slot_stride()` and `zipc_pool_guarded_payload_size()` to size
the backing object. See `docs/GUARD_PAGES.md`. This is overflow detection, not
per-slot ownership isolation: another valid slot remains mapped.
`ZIPC_POOL_F_STRICT_OWNERSHIP` optionally revokes a slot mapping after
ownership transfer on Linux page-protect-capable mappings; see
`docs/OWNERSHIP.md`. It is disabled by default because of mprotect/TLB overhead.

Topology configuration from static C/header data or an INI file is documented in `docs/TOPOLOGY.md`.

This permits
metadata in reserved DDR and slot payloads in PL BRAM.

## Transport backends

Linux userspace includes POSIX message queues, Unix datagrams, FIFO,
shared-ring/eventfd, RPMsg device endpoints, IPI callbacks, PL ring/IRQ,
and callback-mediated SMC/FF-A. FreeRTOS includes queues, task notifications,
RPMsg, IPI, PL ring/IRQ, SMC, and FF-A. Bare metal includes RPMsg, IPI,
PL ring/IRQ, SMC, and FF-A. Xen uses a shared SPSC ring with event channels.
Linux kernel provides kfifo and ring/waitqueue primitives, with callback hooks
for RPMsg, IPI, PL IRQ, SMC, and FF-A integration.

## Build and test on Linux

```bash
make
make test
```

The basic example uses POSIX shared memory, a POSIX message queue, and the high-level link/buffer API. The
integration regression exercises split memory, FIFO, Unix sockets,
ring/eventfd, PL ring/IRQ simulation, SMC/FF-A simulation, loops, prepend, and
append.

## Universal chain

`examples/universal/` documents this requested topology:

```text
FreeRTOS task1 (R5_0)
 -> FreeRTOS task2 (R5_0)
 -> Linux user process1
 -> Linux user process2
 -> Linux kernel workqueue1
 -> Linux kernel workqueue2
 -> Xen Linux guest userspace process1
 -> Xen Linux guest userspace process2
 -> bare-metal R5_1
```

It is intentionally split into native-environment source fragments rather than
pretending this heterogeneous deployment can be linked into one executable.
See `examples/universal/README.md` for the proposed transport at each hop.

## Integration constraints

- Cache maintenance and barriers are platform responsibilities for non-coherent
  A53/R5/PL paths.
- `/dev/mem` cached versus uncached behavior is kernel/architecture-dependent;
  a dedicated driver is preferred for production.
- Do not place zIPC atomic control metadata in AXI BRAM unless atomic semantics
  are explicitly guaranteed.
- Xen guests should use Xen static shared memory plus event channels, not a
  physical ZynqMP IPI for guest-to-guest notification.
- FF-A is normally kernel-mediated on Linux; SMC/FF-A callbacks in the user
  platform are adapters to a driver or ioctl interface.

## Ready-to-play examples

Three integration-oriented examples are available under `examples/ready-to-play/`:

1. **`linux-5-processes/`** — five Linux userspace processes using POSIX shared memory and one shared-ring/eventfd link per hop. This is directly buildable on a standard Linux host without huge-page configuration:

   ```bash
   make ready-linux5
   ./build/zipc-ready-linux5
   ```

2. **`freertos-5-tasks/`** — five FreeRTOS tasks on R5_0 using Normal non-cacheable reserved memory and direct-to-task notifications. It requires only one reserved R5 memory region and no OpenAMP/IPI setup.

3. **`linux-r5-chain/`** — Linux process 1 -> FreeRTOS R5_0 -> bare-metal R5_1 -> Linux process 2, using one DT reserved-memory pool, RPMsg for Linux/R5 links, and ZynqMP IPI between the R5 cores.

The FreeRTOS and heterogeneous examples include complete zIPC flow code plus intentionally small board/BSP hooks for memory attributes, OpenAMP endpoints, and IPI signaling.

## Utilities and benchmarks

Build all Linux utilities with:

```sh
make utilities
```

- `zipc-ping`: RTT, forward one-way latency, and per-relay hop latency.
- `zipc-membench`: throughput comparison for POSIX SHM, hugetlbfs, and DT reserved-memory mappings.
- `zipc-packetrate`: maximum packet rate for ring/eventfd, FIFO, Unix datagram, and POSIX mqueue transports.
- `zipc-stat`: inspect pool counters, component epochs/heartbeats, active slots, recovery counts, and per-slot traces.

See [`utilities/README.md`](utilities/README.md) for command-line examples and measurement semantics.

## v0.1.1 target-oriented integration tests

Two host-buildable integration harnesses exercise the real target platform adapters:

```bash
make integration-targets
./build/zipc-integration-freertos
./build/zipc-integration-baremetal
```

- `tests/integration-freertos.c` validates DT-reserved-memory mapping, the FreeRTOS queue transport, component epochs, slot transfer, and the high-level link/buffer API.
- `tests/integration-baremetal.c` validates DT-reserved-memory mapping, the callback-driven IPI transport, component epochs, slot transfer, and the high-level link/buffer API.

The stubs under `tests/stubs/` provide only enough FreeRTOS/OpenAMP behavior for host regression. Target validation still requires the actual FreeRTOS BSP, OpenAMP stack, cache policy, IPI driver, and hardware memory map.

### Polling event backend

For latency-critical links, zIPC supports `ZIPC_EVENT_BACKEND_SHM_POLLING` via
`ZIPC_TRANSPORT_SHM_RING_POLLING`. It uses the same shared SPSC descriptor ring
as the eventfd adapter but the receiver polls the shared producer index directly.
This avoids wakeup/syscall latency at the cost of continuously consuming CPU.
Use it for a dedicated worker/core; use eventfd, task notifications, IPI, or IRQ
when idle efficiency is more important.
