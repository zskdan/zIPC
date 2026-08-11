# Ready to play: five Linux userspace processes

## Backend composition

```text
payload backend:    POSIX SHM
descriptor backend: shared SPSC ring
event backend:      eventfd
```


This example creates five child processes and chains one zIPC buffer:

```text
P0 -> P1 -> P2 -> P3 -> P4
```

It uses:

- `ZIPC_SHM_POSIX` for the slot pool. This requires no huge-page boot or sysctl configuration.
- One `ZIPC_TRANSPORT_SHM_RING_EVENTFD` SPSC link between each adjacent process.
- `fork()` so the anonymous shared rings and eventfds are inherited by all processes.

Build and run from the project root:

```bash
make ready-linux5
./build/zipc-ready-linux5
```

Expected output:

```text
P0: P0
P1: P0->P1
P2: P0->P1->P2
P3: P0->P1->P2->P3
P4: P0->P1->P2->P3->P4
final: P0->P1->P2->P3->P4
hop_count=5 distinct=5
```

Each process prints the cumulative payload as soon as it has added its own
stage, before forwarding it to the next process.
