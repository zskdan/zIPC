# NNG to zIPC migration

## Stage 1: minimal code change

Map each existing NNG component-to-component connection to one zIPC link.

```text
CompA -- NNG AB --> CompB -- NNG BC --> CompC
                  becomes
CompA -- zIPC AB -> CompB -- zIPC BC -> CompC
```

Start with `zipc_send_copy()` / `zipc_recv_copy()`. This preserves a familiar
copy-oriented model and minimizes application restructuring.

## Stage 2: message/buffer ownership

Replace copied payload calls with `zipc_buffer_t`:

```text
nng_msg *             -> zipc_buffer_t
nng_msg_body()        -> zipc_buffer_data()
nng_msg_len()         -> zipc_buffer_size()
nng_sendmsg()         -> zipc_send()
nng_recvmsg()         -> zipc_recv()
nng_msg_free()        -> zipc_buffer_release()
```

The key semantic difference is explicit ownership: successful `zipc_send()`
consumes the sender's buffer object.

## Stage 3: zero-copy relay

When AB and BC accept the same payload pool, CompB can receive from AB, modify
that same slot, and send it through BC without copying. See
`examples/shared-buffer-chain/`.

Pools are independent topology objects rather than link-owned storage. Links
carry descriptors/notifications and reference a compatible pool. A pool may be
scoped to exactly the set of components that need zero-copy access; it does not
need to be system-global.
