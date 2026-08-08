# zIPC utilities

These Linux userspace utilities exercise the public zIPC API and are intended
for bring-up, regression testing, and platform comparison.

## Build

```sh
make utilities
```

## `zipc-ping`

Measures round-trip latency and forward one-way latency between two components.
Optional relay processes record arrival/departure timestamps, allowing per-hop
latency reporting.

```sh
./build/zipc-ping --count 100
./build/zipc-ping --relays 3 --count 100
```

The utility uses POSIX shared memory and shared SPSC rings with eventfd
notification. All Linux processes use `CLOCK_MONOTONIC_RAW`, so one-way and
per-hop timestamps share the same host clock domain.

Output fields:

- `rtt`: source send to source reply reception.
- `forward`: source departure to final destination arrival.
- `hops`: one-way latency for each forward link.
- `hop_count`: total zIPC component claims, including the return path.

For cross-processor or cross-machine one-way measurements, the participating
clocks must be synchronized. RTT does not require clock synchronization.

## `zipc-membench`

Measures sequential writes and cache-line-stride reads through a selected zIPC
memory backend.

```sh
./build/zipc-membench --backend posix --size 64M --iterations 20
./build/zipc-membench --backend hugepages \
    --path /dev/hugepages/zipc_bench --size 64M
sudo ./build/zipc-membench --backend dtrevmem-cached \
    --phys 0x70000000 --size 64M
sudo ./build/zipc-membench --backend dtrevmem-uncached \
    --phys 0x70000000 --size 64M
```

Supported backends:

- `posix`
- `hugepages`
- `dtrevmem-cached`
- `dtrevmem-uncached`

The DT-reserved-memory modes default to `/dev/mem`; `--path` selects a dedicated
character/UIO device instead. Results depend strongly on cache attributes,
NUMA placement, page faults, and platform clocking.

## `zipc-packetrate`

Measures the maximum sustained descriptor/packet rate of a selected Linux
transport backend while the payload remains in the zIPC slot pool.

```sh
./build/zipc-packetrate --transport ring-eventfd \
    --packets 1000000 --payload 8
./build/zipc-packetrate --transport fifo --packets 100000
./build/zipc-packetrate --transport unix-dgram --packets 100000
./build/zipc-packetrate --transport mqueue --packets 100000
```

Supported transports:

- `ring-eventfd`
- `fifo`
- `unix-dgram`
- `mqueue`

The reported packet rate includes zIPC allocation, ownership transfer, claim,
and release, not just the underlying OS notification primitive.


## zipc-stat

Inspect a live POSIX-shared-memory pool created with matching geometry:

```sh
./build/zipc-stat --name /zipc_pool --slots 64 --capacity 4096
```

Add `--all` to include free slots. The output contains pool counters, component epochs and heartbeats, slot owner/age/hop limits, recovery counts, and the retained trace entries.


## Backend terminology

Use the canonical three-role model: payload backend, descriptor backend, and event backend. See `docs/BACKENDS.md`.
