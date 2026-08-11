# Shared-buffer chain

This example demonstrates the migration-oriented zIPC model:

```text
CompA -- link AB --> CompB -- link BC --> CompC
          \________ same buffer ________/
```

All components use the same payload pool. AB and BC are independent links with
independent descriptor/event transport resources. Ownership moves between the
components; the payload is not copied.

The components modify fixed locations using the public checked accessor:

```text
CompA : offset 0x00
CompB : offset 0x40
CompC : offset 0x80
```

For example:

```c
uint32_t *field = zipc_buffer_at(&buffer, 0x40, sizeof(*field));
if (field == NULL)
    return error;
*field = value;
```

`zipc_buffer_at()` uses an absolute offset from the beginning of the complete
buffer storage. It does not depend on the current logical `data` position, so
prepend/trim operations do not change fixed field offsets.

The example also demonstrates the two topology configuration sources. With no
argument it registers the compiled `zipc_config.h` topology:

```sh
./build/examples/zipc-shared-buffer-chain
```

On hosted/Linux systems the same data path can instead load the equivalent INI
configuration:

```sh
./build/examples/zipc-shared-buffer-chain \
    --config examples/shared-buffer-chain/zipc.conf
```

The application send/receive/buffer logic is identical in both modes.

Each component prints its view of the shared buffer as ownership passes through
the chain. The handle is an opaque generation/slot identifier generated at
runtime, not a process address:

```text
shared-buffer chain: one buffer flows A -> B -> C, never copied
offsets: A@0x00 B@0x40 C@0x80 via zipc_buffer_at(); handle must be identical at every stage
A: send handle=0x<runtime-handle> A=aaaaaaaa
B: forward handle=0x<runtime-handle> A=aaaaaaaa B=bbbbbbbb
C: recv handle=0x<runtime-handle> A=aaaaaaaa B=bbbbbbbb C=cccccccc
PASS: same handle 0x<runtime-handle> across all stages; fixed offsets 0x00/0x40/0x80 read consistently
```
