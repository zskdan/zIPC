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
zipc_buffer_alloc(link, payload_size, &b);
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

Successful `zipc_send()` and `zipc_buffer_release()` invalidate the local object.
Subsequent access, send, or release is rejected.

## Allocation

`zipc_buffer_alloc()` is the common API. The requested size becomes the initial
logical data size, beginning at buffer offset zero.

`zipc_buffer_alloc_ex()` is available when a protocol intentionally reserves
headroom and/or tailroom.

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
for example `"0.1.10"`. It is safe to call at any time and the returned pointer
is valid for the lifetime of the library.
