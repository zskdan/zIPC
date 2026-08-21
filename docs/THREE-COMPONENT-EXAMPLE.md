# Three-component ownership walkthrough

The first half of the existing
[`shared-buffer-chain`](https://github.com/zskdan/zIPC/tree/master/examples/shared-buffer-chain)
example is the smallest useful relay topology:

```text
A (producer) -> B (relay) -> C (consumer)
```

The example runs these as logical components in one Linux process so it is easy
to build and inspect. In a deployment, the same ownership flow can cross
processes, operating systems, or processors as long as the selected memory and
transport backends satisfy zIPC's platform requirements.

## What the example creates

All three components use one shared buffer pool. Two directional links carry
small buffer descriptors:

```text
shared pool: stores the payload bytes

A -- link AB --> B -- link BC --> C
```

The example names the local endpoint objects after the link and component:

- `ab_a`: A's sending endpoint for link AB;
- `ab_b`: B's receiving endpoint for link AB;
- `bc_b`: B's sending endpoint for link BC;
- `bc_c`: C's receiving endpoint for link BC.

B needs two endpoint objects because it receives on one directional link and
sends on another.

## Step 1: A allocates and sends

A allocates a root buffer from the pool, fills its payload, and sends it to B:

```c
zipc_buffer_t root;
zipc_buffer_alloc(ab_a, 320U, &root, NULL);

for (uint32_t i = 0U; i < 320U; ++i)
    ((uint8_t *)zipc_buffer_data(&root))[i] = (uint8_t)i;

zipc_send(ab_a, &root);
```

Passing `NULL` as the final allocation argument means this is a root buffer,
not a child of another buffer. After `zipc_send()` succeeds, A must not access
`root`: sending consumes A's local buffer object and transfers ownership toward
B.

## Step 2: B relays the same buffer

B receives the buffer from AB and sends it onward through BC:

```c
zipc_recv(ab_b, &root);

/* B owns the buffer here and may inspect or modify its payload. */

zipc_send(bc_b, &root);
```

This relay does not allocate a second payload buffer and does not copy the 320
payload bytes. The payload remains in the same shared-pool slot. Only a small
descriptor is transferred between components, and the immutable
`zipc_buffer_id()` remains unchanged.

After B sends successfully, its local `root` object is invalid for the same
reason A's was: B no longer owns the buffer.

## Step 3: C receives and finishes

C receives the same authoritative buffer:

```c
zipc_recv(bc_c, &root);

printf("C received buffer id=0x%016" PRIx64 " size=%zu\n",
       zipc_buffer_id(&root), zipc_buffer_size(&root));

zipc_buffer_release(&root);
```

A minimal three-component program can release the buffer at this point,
returning its slot to the pool. The complete shipped example continues instead:
C creates child buffers from the root's data and sends those children to D.
Those child copies are explicit application work; the A-to-B-to-C relay itself
is zero-copy.

## Ownership at each operation

| Operation | Application allowed to access the payload | Slot state |
| --- | --- | --- |
| A allocates | A | `OWNED` |
| A sends to B | None until B receives | `TRANSFER` |
| B receives | B | `OWNED` |
| B sends to C | None until C receives | `TRANSFER` |
| C receives | C | `OWNED` |
| C releases | None; the slot is reusable | `FREE` |

The important rule is that an application may access a buffer only while its
component owns a valid local buffer object. A successful send or release
invalidates that object.

## Build and run it

From the repository root:

```sh
make shared-buffer-chain
./build/examples/zipc-shared-buffer-chain
```

The first lines show the three-component relay before the example moves on to
its child-buffer demonstration:

```text
A: created root buffer ...
A: sent root buffer ...
B: relayed root buffer ...
C: received root buffer ...
```

The buffer ID printed by A, B, and C should be identical. That stable identity,
together with the absence of a new allocation at B, demonstrates that the same
shared buffer was relayed rather than copied.
