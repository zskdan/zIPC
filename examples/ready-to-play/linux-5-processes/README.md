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
./build/examples/zipc-ready-linux5
```

Expected output (PIDs vary):

```text
memory: opened /zipc_rtp_linux5 (262144 bytes, POSIX shm)
pool: 32 slots x 4096 bytes, payload at offset 16384
links: 4 ring/eventfd links, P0 -> P1 ... P3 -> P4
spawned P0 pid=<pid>
...
P0[pid=<pid>]: P0
P1[pid=<pid>]: P0->P1
P2[pid=<pid>]: P0->P1->P2
P3[pid=<pid>]: P0->P1->P2->P3
P4[pid=<pid>]: P0->P1->P2->P3->P4
final: P0->P1->P2->P3->P4
hop_count=5 distinct=5
all 5 processes exited cleanly
cleanup: links destroyed, shm unlinked
```

Each process prints the cumulative payload as soon as it has added its own
stage, before forwarding it to the next process.
