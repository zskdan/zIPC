# zIPC application API

## Design goal

The application API is intentionally small and NNG-like. Pool construction,
SPSC rings, eventfd/IPI wiring, cache policy, and platform memory are integration
concerns. Message-processing code should normally deal only with named links and
opaque `zipc_buffer_t` objects.

## Buffer model

A `zipc_buffer_t` represents one owned buffer storage region plus a logical data
window inside that storage:

```text
buffer storage
+---------------------------------------------------------+
| headroom |           logical data           | tailroom |
+---------------------------------------------------------+
^          ^                                  ^           ^
offset 0   zipc_buffer_data()                  |           end
           <-------- zipc_buffer_size() ------>
```

The word *slot* is an internal allocation concept and is intentionally not part
of the normal application buffer API.

## Buffer lifecycle

A `zipc_buffer_t` is opaque and stack-allocatable. Applications must not copy or
inspect its representation.

Producer:

```c
zipc_buffer_t b;
zipc_buffer_alloc(link, payload_size, &b, NULL);
fill(zipc_buffer_data(&b));
zipc_send(link, &b);          /* consumes b on success */
```

Relay:

```c
zipc_recv(from_a, &b);
modify(zipc_buffer_data(&b));
zipc_send(to_c, &b);          /* same buffer, new link */
```

Final consumer:

```c
zipc_recv(link, &b);
consume(zipc_buffer_data(&b), zipc_buffer_size(&b));
zipc_buffer_release(&b);      /* consumes b on success */
```

`zipc_send()` invalidates the local object on `ZIPC_OK` and on
`ZIPC_ERR_TRANSPORT_PUBLISHED`. The latter means the descriptor became visible
before a later notification failed, so ownership cannot safely roll back and
the slot remains `TRANSFER`. Ordinary `ZIPC_ERR_TRANSPORT` means publication did
not occur; sender ownership is restored and the buffer remains valid.
Successful `zipc_buffer_release()` also invalidates the local object.
Subsequent access, send, or release of an invalidated object is rejected.

## Allocation

`zipc_buffer_alloc()` is the common API. The requested size becomes the initial
logical data size, beginning at buffer offset zero.

`zipc_buffer_alloc_ex()` is available when a protocol intentionally reserves
headroom and/or tailroom.

Both allocation functions take a final optional `const zipc_buffer_t *parent`.
`NULL` creates a root (`parent_id == 0`). A valid owned parent is not changed or
consumed, may reside in another pool, and contributes only its immutable ID.
Each child receives a new unique ID. Use `zipc_buffer_id()` and
`zipc_buffer_parent_id()` to inspect lineage.

Buffer IDs pack allocator component (8 bits), random nonzero runtime session
(24 bits), and allocation sequence (32 bits). Sequence zero is used. The last
issued sequence is `UINT32_MAX - 1`; `UINT32_MAX` is an internal exhaustion
sentinel that rotates to a distinct session and resumes at zero. Gaps are
permitted; IDs containing sequence `UINT32_MAX` are invalid. Entropy failure
returns `ZIPC_ERR_ENTROPY_UNAVAILABLE` and the claimed
slot is returned to the pool.

The process-global generator does not detect `fork()`. Fork before first use of
a component generator, `exec()` one side, or assign distinct component IDs;
continuing the same initialized generator in both parent and child can duplicate
IDs because process-local atomic state is copied by `fork()`.

## Recommended buffer API

```c
void   *zipc_buffer_data(zipc_buffer_t *buffer);
size_t  zipc_buffer_size(const zipc_buffer_t *buffer);
uint32_t zipc_buffer_headroom(const zipc_buffer_t *buffer);
uint32_t zipc_buffer_tailroom(const zipc_buffer_t *buffer);

void *zipc_buffer_at(zipc_buffer_t *buffer,
                     size_t offset,
                     size_t length);

zipc_status_t zipc_buffer_append(zipc_buffer_t *buffer,
                                 const void *data,
                                 uint32_t length);
zipc_status_t zipc_buffer_prepend(zipc_buffer_t *buffer,
                                  const void *data,
                                  uint32_t length);
zipc_status_t zipc_buffer_trim_front(zipc_buffer_t *buffer,
                                     uint32_t length);
zipc_status_t zipc_buffer_trim_back(zipc_buffer_t *buffer,
                                    uint32_t length);
```

### `zipc_buffer_at()`

`zipc_buffer_at()` addresses the complete buffer storage using an **absolute
offset from buffer offset zero**. It is not relative to `zipc_buffer_data()`.

```c
struct component_b_fields *b =
    zipc_buffer_at(&buffer, 0x40, sizeof(*b));

if (b == NULL)
    return ZIPC_ERR_REGION_OVERFLOW;

b->status = 1;
```

The function returns `NULL` when the local buffer object is invalid or when the
complete requested range does not fit in the buffer storage. The check is
performed without overflow-prone `offset + length` arithmetic.

The absolute origin is stable even if the logical window is later prepended or
trimmed. This is useful for chained fixed-layout processing:

```text
CompA writes offset 0x00
CompB writes offset 0x40
CompC writes offset 0x80
```

### Append and prepend

`zipc_buffer_append()` copies bytes into tailroom and extends the logical data
window at the back. `zipc_buffer_prepend()` copies bytes into headroom and moves
the logical data start toward buffer offset zero. Neither operation reallocates
the buffer.

### Trim front and back

`zipc_buffer_trim_front(buffer, n)` performs logically:

```text
data     += n
size     -= n
headroom += n
```

No payload bytes are moved. The removed bytes remain physically present in the
buffer storage.

`zipc_buffer_trim_back(buffer, n)` performs logically:

```text
size     -= n
tailroom += n
```

Again, no payload bytes are moved. Both functions reject trimming more than the
current logical size.

## Compatibility buffer APIs

The following v0.x entry points remain available for source migration but are
not part of the recommended application interface:

- `zipc_buffer_get()` / `zipc_buffer_put()`
- `zipc_receive()` / `zipc_receive_timeout()`
- `zipc_buffer_length()`
- `zipc_buffer_capacity()`
- `zipc_buffer_resize()`
- `zipc_buffer_const_data()`

New application code should use the smaller API above.

See [`examples/buffer-api/`](../examples/buffer-api/) for a runnable walkthrough
of allocation with headroom, append, prepend, absolute offset access, trimming,
bounds validation, and ownership invalidation.

## Named links

Platform/integration code binds runtime resources by name and registers a
declarative topology with `zipc_topology_register_config()`, or loads the same
model from a string/file. Application code can then use:

```c
zipc_link_t *link;
zipc_link_open(&link, "ab");
```

A topology table is process-local. `zipc_link_create()` remains the expert API
when dynamic or platform-specific configuration is required.

## Copy migration API

For the first stage of NNG migration:

```c
zipc_send_copy(link, data, size);
zipc_recv_copy(link, data, capacity, &actual_size);
```

These helpers deliberately copy. They simplify initial porting; the owned buffer
API should be used once the application is ready to transfer ownership.

## Topology sources

The normal application API opens a named link with `zipc_link_open()`. Runtime
pool and transport objects are first bound by stable names with
`zipc_topology_bind_pool()` and `zipc_topology_bind_transport()`. The declarative
topology can then be registered from static C/header data with
`zipc_topology_register_config()`, parsed from memory with
`zipc_topology_load_string()`, or loaded on hosted/Linux systems with
`zipc_topology_load_file()`. All paths use `zipc_topology_validate()`.
See `TOPOLOGY.md` for the configuration format and merge policies.

## Library version

Compile-time version macros:

- `ZIPC_VERSION_MAJOR`, `ZIPC_VERSION_MINOR`, `ZIPC_VERSION_PATCH` — numeric
  semantic-version components.
- `ZIPC_VERSION_STRING` — canonical version string.

Runtime version query:

```c
const char *version = zipc_version_string();
```

`zipc_version_string()` always returns the immutable library version string,
for example `"0.3.0"`. It is safe to call at any time and the returned pointer
is valid for the lifetime of the library.

## Timeout compatibility APIs

ABI 3 transports expose only send and receive operations; they do not expose a
per-call timed operation. Consequently `zipc_send_timeout()` and
`zipc_recv_timeout()` retain their existing signatures but the
`timeout_ticks` argument cannot override an opened transport. Blocking and
timeout behavior comes from the transport configuration used at link creation,
such as `poll_timeout_ns` for Linux shared-memory polling or transport
`timeout_ticks` on FreeRTOS. A value accepted by the compatibility API is not a
portable duration contract. Uniform per-call timeout semantics require the
planned later transport contract and are not claimed by v0.3.0.

## Online component restart recovery

Pool ABI 3 provides a replacement relay with an online, caller-driven recovery
sequence:

```c
zipc_restart_t restart;
zipc_component_restart_begin(pool, component, dead_epoch, &restart);

/* Reopen each local link using restart.epoch, then reconcile it. */
zipc_link_reconcile(input, &restart, &recovered_input);
zipc_link_reconcile(output, &restart, &recovered_output);

while (zipc_component_restart_next(&restart, &buffer) == ZIPC_OK) {
    handle_from_start(&buffer);
    zipc_send(output, &buffer);
}

zipc_component_restart_finish(&restart);
```

The caller must establish that the exact runtime identified by `dead_epoch` has
terminated before calling `zipc_component_restart_begin()`. Begin atomically
advances the packed component lifecycle to the next epoch in `RECOVERING`; it is
not a mechanism for safely running old and replacement instances concurrently.

`zipc_link_reconcile()` requires a link configured with the recovering local
epoch, stable nonzero `link_id`, and producer or consumer role. In v0.3.0 it
supports only `ZIPC_TRANSPORT_SHM_RING_EVENTFD` and
`ZIPC_TRANSPORT_SHM_RING_POLLING`. Other transports return
`ZIPC_ERR_RECOVERY_UNSUPPORTED`. The ring must be supplied through
`transport.platform_handle`; an anonymously allocated process-local transport
cannot be reattached by a replacement runtime.

`zipc_component_restart_next()` returns each stable `OWNED` buffer belonging to
the old epoch and returns `ZIPC_ERR_NO_BUFFER` after the scan. Adoption preserves
the immutable `buffer_id`, parent ID, payload, and logical data window while
changing owner epoch and bumping handle generation. Old high-level buffers and
links are epoch-fenced and must not be reused.

The replacement invokes the application handler from its beginning for every
adopted buffer. External side effects are therefore at-least-once; applications
that require idempotence must deduplicate by `buffer_id`. There is no persistent
per-cell journal or endpoint lease. `zipc_component_restart_finish()` changes
the lifecycle from `RECOVERING` to `ACTIVE` only after
`zipc_component_restart_next()` has returned `ZIPC_ERR_NO_BUFFER`. The caller is
responsible for reconciling every local recoverable link before finishing.

Recovery-enabled applications must register components and use nonzero epochs.
Epoch-zero compatibility links remain available to legacy applications but are
outside the online-restart guarantee. A second crash while the replacement is
itself in `RECOVERING` is not recoverable online in v0.3.0 and requires the
quiesced administrative recovery path.

## Abandoned claim recovery

`zipc_pool_recover_claiming(pool, recovered_out)` is an administrative recovery
operation. Before calling it, the integrator must externally quiesce every
protocol operation on the pool: allocation, send including rollback,
receive/claim, release, owner recovery, format/reset/shutdown, and another
claiming recovery. Under that condition every remaining `CLAIMING` slot is
abandoned, including a crash immediately after the state CAS before timestamp
or claimant metadata was written.

The sweep does not use `acquired_ns` and does not attribute partial metadata to
a component. It increments only the pool recovery counter. If payload
protection fails, the affected slot is restored to `CLAIMING` without a
completed recovery trace or counter and may be retried while still quiesced.

`zipc_pool_recover_owner()` has the same pool-wide quiescence requirement. It
transiently acquires candidate states before validating owner-protected
component, epoch, and age metadata, so it must not overlap allocation, send,
receive, release, or another recovery operation.

## Trace Hook

`zipc_trace_set_hook()` installs an optional process-local synchronous callback.
The fixed `zipc_trace_record_t` includes timestamp, event, buffer ID, parent ID,
handle, pool ID, component ID, and transfer sequence. Allocate, send, receive,
release, recover, and error paths emit records. The hook must not mutate the
traced buffer, allocate implicitly on behalf of zIPC, block protocol progress,
or assume a worker thread; it runs synchronously in the caller context. The hook
is non-reentrant and must not call any zIPC API. Configure or replace it only
while all protocol operations are externally quiesced.
