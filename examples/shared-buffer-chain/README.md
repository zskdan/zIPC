# Buffer lineage chain

This host example runs the topology:

```text
A -> B -> C -> D
```

A creates one 320-byte root buffer. B relays the same authoritative slot. C
receives the root and creates five independent 64-byte child buffers, passing
the still-owned root as each allocation's parent. C then releases the root and
sends all children to D, which receives and releases them.
Before releasing each child, D validates its immutable ID, root parent, 64-byte
size, and all copied payload bytes. Success ends with a `PASS` line. Every action
line includes component, action, buffer kind, ID, parent, and size.

The child payload bytes are copied from the corresponding root slices because
each child is a separate allocation. zIPC does not copy bytes while relaying the
root or transferring any child.

Run the compiled topology:

```sh
./build/examples/zipc-shared-buffer-chain
```

Or load the equivalent INI topology:

```sh
./build/examples/zipc-shared-buffer-chain \
    --config examples/shared-buffer-chain/zipc.conf
```

The example uses eight pool slots. At peak, one root plus five children are
owned, so the pool and ring capacities remain safe.
