# Guarded payload slots

zIPC can place one inaccessible virtual-memory page after every payload slot on
Linux POSIX shared memory. This is intended to turn common linear buffer
overflows into an immediate `SIGSEGV`/`SIGBUS` instead of silently corrupting
the following slot.

## Layout

For a 64 KiB slot and a 4 KiB host page:

```text
| slot 0: 64 KiB RW | 4 KiB PROT_NONE |
| slot 1: 64 KiB RW | 4 KiB PROT_NONE |
| slot 2: 64 KiB RW | 4 KiB PROT_NONE |
```

The resulting slot stride is 68 KiB. The guard pages consume virtual address
space but are not usable payload memory.

## Configuration

Guard pages are a pool property, not a link property. All links can keep using
the same API. Each process attaching the payload mapping applies the guard
protection locally.

```c
const uint32_t slot_count = 128;
const uint32_t slot_capacity = 64U * 1024U;

size_t payload_size =
    zipc_pool_guarded_payload_size(slot_count, slot_capacity);

zipc_platform_memory_config_t payload_cfg = {
    .type = ZIPC_SHM_POSIX,
    .size = payload_size,
    .backend.posix = {
        .name = "/zipc-payload",
        .create = true,
        .unlink_on_close = false,
    },
};

zipc_pool_config_t pool_cfg = {
    .control_memory = control_memory,
    .payload_memory = payload_memory,
    .slot_count = slot_count,
    .slot_capacity = slot_capacity,
    .slot_stride = 0, /* zIPC derives slot_capacity + one page */
    .payload_alignment = 64,
    .flags = ZIPC_POOL_F_GUARD_PAGES,
};
```

`slot_capacity` and `payload_offset` must be page aligned. The initial
implementation supports guard pages on the Linux POSIX shared-memory backend.
Other platform backends return `ZIPC_ERR_UNSUPPORTED_MEMORY` when this flag is
requested.

## Protection scope

This feature protects against contiguous overflow across the end of the owned
slot, for example:

```c
memcpy(buffer.data, src, buffer.capacity + 1);
```

The first write into the guard page faults immediately.

It does **not** provide ownership isolation between valid slots. Code that
explicitly computes the address of another mapped slot can still access it.
zIPC's ownership checks and bounded buffer API remain responsible for normal
logical access control.

## Regression test

```bash
make guard-pages
./build/tests/zipc-guard-pages-linux
```

The test verifies that:

- writing exactly within the 64 KiB slot succeeds;
- writing the first byte after the slot faults in a child process;
- the next valid slot remains mapped and accessible;
- `zipc_buffer_set_region()` still rejects a region larger than the slot.

## Ownership protection modes

Guard pages and ownership protection solve different classes of bugs and must
not be confused.

### Normal mode

Normal mode is intended for production use where low transfer latency is
important. zIPC enforces ownership at the API/protocol level:

- a buffer object is valid only while the local component owns it;
- a successful send transfers ownership and the sender-side buffer object
  should be treated as invalid;
- release consumes the local ownership reference;
- stale handles are rejected using the slot generation;
- bounded buffer operations reject accesses beyond `slot_capacity`;
- optional guard pages fault on linear overflow past the end of a slot.

Normal mode does not revoke the process virtual-memory mapping for a slot after
ownership is transferred. If application code retains a raw data pointer and
uses that pointer after `zipc_send()`, the operating system cannot distinguish
that stale pointer from a valid access to the still-mapped shared-memory page.
The zIPC API can detect use of an invalidated `zipc_buffer_t`, but it cannot
prevent direct access through a previously retained raw pointer.

### Strict ownership protection mode (optional; implemented on Linux)

`ZIPC_POOL_F_STRICT_OWNERSHIP` enforces ownership with page permissions in
addition to the normal API checks on Linux page-protect-capable mappings. When
ownership moves from component B to component C, B's local mapping of that slot
is changed to `PROT_NONE`; after C claims the descriptor, C makes its local
mapping readable/writable. A stale raw pointer in B therefore faults on access.

Conceptually:

```text
Before B -> C:
    B: slot 42 = RW
    C: slot 42 = inaccessible

After B -> C:
    B: slot 42 = PROT_NONE
    C: slot 42 = RW
```

This mode is implemented but intentionally optional because changing page permissions on each
ownership transfer can add system-call/TLB-management overhead and therefore
increase IPC latency. It is primarily intended for development, validation,
and safety-focused deployments rather than as the default fast path.

Strict ownership protection is independent of guard pages. A deployment may
use either or both:

```text
Normal + guard pages:
    catches API misuse, stale handles, bounds errors, and linear slot overflow

Strict ownership protection:
    additionally catches access through stale raw pointers after ownership
    transfer, provided each protected slot can be controlled independently by
    the platform MMU/MPU mechanism
```

Strict mode is implemented in zIPC v0.1.7 as `ZIPC_POOL_F_STRICT_OWNERSHIP` for Linux payload mappings that support page protection. See `docs/OWNERSHIP.md` for requirements and performance caveats.
