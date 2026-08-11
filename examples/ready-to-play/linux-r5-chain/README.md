# Ready to play: Linux P1 -> FreeRTOS R5_0 -> bare-metal R5_1 -> Linux P2

## Backend composition

```text
payload backend:    reserved DDR
descriptor backend: RPMsg / shared mailbox
event backend:      RPMsg notification / IPI
```


The chain uses one fixed reserved DDR slot pool:

```text
Linux process 1 --RPMsg--> FreeRTOS R5_0 --IPI--> bare-metal R5_1 --RPMsg--> Linux process 2
```

Recommended mechanisms with minimal platform changes:

- Shared memory: one DT `reserved-memory` region at `0x70000000`, mapped by Linux through a small `/dev/zipc-shm` driver and directly by both R5 cores.
- Linux mappings: `ZIPC_SHM_DTREVMEM_CACHED`.
- R5 mappings during initial bring-up: `ZIPC_SHM_DTREVMEM_UNCACHED` as Normal non-cacheable memory.
- Linux <-> R5: existing OpenAMP/RPMsg infrastructure.
- R5_0 -> R5_1: one ZynqMP IPI channel and a shared `zipc_message_t` mailbox.

## Device tree

```dts
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;

    zipc_shm: zipc@70000000 {
        no-map;
        reg = <0x0 0x70000000 0x0 0x00400000>;
    };
};
```

The Linux driver should map the region as normal cached memory and expose `/dev/zipc-shm`. The R5 MPU should map the same range as Normal non-cacheable for the first integration. If cached mappings are later enabled on the R5, add ownership-boundary cache clean/invalidate operations.

## Files

- `linux/process1.c`: formats the pool once, writes the initial payload, and sends to R5_0.
- `freertos-r5_0/main.c`: receives over RPMsg, appends `->R5_0`, and sends through IPI.
- `baremetal-r5_1/main.c`: receives through IPI, appends `->R5_1`, and sends through RPMsg.
- `linux/process2.c`: receives, prints, and releases the slot.

The board-specific symbols declared with `extern` are intentionally small integration hooks: OpenAMP endpoints/receive queues and IPI send/wait callbacks.

## Console output

Each stage prints the cumulative payload before forwarding it. The messages
appear on each component's own console: Linux-P1 and Linux-P2 print on the A53
terminal, while R5_0 and R5_1 print through their BSP UART channels.

```text
[Linux-P1] Linux-P1
[R5_0]     Linux-P1->R5_0
[R5_1]     Linux-P1->R5_0->R5_1
[Linux-P2] final: Linux-P1->R5_0->R5_1
```
