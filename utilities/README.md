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
./build/utilities/zipc-ping --count 100
./build/utilities/zipc-ping --relays 3 --count 100 --interval 0.25 --timeout 2
```

Probes are sent at one-second intervals by default. `--interval 0` disables the
delay, and `--timeout` controls the bounded reply wait (five seconds by
default). Invalid, negative, non-finite, and partially parsed values are
rejected.

The utility uses POSIX shared memory and shared SPSC rings with eventfd
notification. All Linux processes use `CLOCK_MONOTONIC_RAW`, so one-way and
per-hop timestamps share the same host clock domain.

Output fields:

- `time`: round-trip time from source send to source reply reception.
- `forward`: source departure to final destination arrival.
- `hops`: one-way latency for each forward link.
- `hop_count`: total zIPC component claims, including the return path.

The final summary reports transmitted and received probes, packet loss, and
round-trip minimum/average/maximum in microseconds.

For cross-processor or cross-machine one-way measurements, the participating
clocks must be synchronized. RTT does not require clock synchronization.

## `zipc-membench`

Measures sequential writes and cache-line-stride reads through a selected zIPC
memory backend.

```sh
./build/utilities/zipc-membench --backend posix --size 64M --iterations 20
./build/utilities/zipc-membench --backend hugepages \
    --path /dev/hugepages/zipc_bench --size 64M
sudo ./build/utilities/zipc-membench --backend dtrevmem-cached \
    --phys 0x70000000 --size 64M
sudo ./build/utilities/zipc-membench --backend dtrevmem-uncached \
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
./build/utilities/zipc-packetrate --transport ring-eventfd \
    --packets 1000000 --payload 8
./build/utilities/zipc-packetrate --transport ring-eventfd \
    --packets 10000 --payload 64K --relays 3
./build/utilities/zipc-packetrate --transport ring-eventfd \
    --packets 1000 --payload 1M --relays 3
./build/utilities/zipc-packetrate --transport fifo --packets 100000
./build/utilities/zipc-packetrate --transport unix-dgram --packets 100000
./build/utilities/zipc-packetrate --transport mqueue --packets 100000
```

Supported transports:

- `ring-eventfd`
- `fifo`
- `unix-dgram`
- `mqueue`

The reported packet rate includes zIPC allocation, ownership transfer, claim,
and release, not just the underlying OS notification primitive. `--relays N`
adds N forwarding processes between the producer and consumer; the default is
zero. Relays forward the same buffer without copying, and only the consumer
releases it. Output reports end-to-end packet rate and the aggregate transfer
rate across all `N + 1` transport hops.

`--payload` accepts decimal byte counts or `K`, `M`, and `G` binary suffixes.
Slot capacity is derived from the payload, and slot stride is aligned to 64
bytes. Unless `--slots` is specified, the utility selects at most 256 slots
while targeting a 32 MiB payload pool. For example, 64 KiB, 256 KiB, and 1 MiB
payloads select 256, 128, and 32 slots respectively. Explicit `--slots N`
overrides this automatic selection and therefore controls the shared pool's
global in-flight capacity.

For `ring-eventfd`, each ring is internally sized to the selected slot count;
ring depth is not an independent benchmark option. The warm-up covers up to
1000 packets, at least one automatic pool cycle where practical, and targets
about 64 MiB of payload writes for large payloads.

### Packet-rate scaling analysis

`zipc-packetrate-analyze.py` builds the utility and runs a repeatable relay
scaling matrix. Its default experiment uses a 11200-byte payload, one million
packets, three repetitions, relay counts from 0 through 100, and zero-payload
controls at 0 and 100 relays:

```sh
python3 utilities/zipc-packetrate-analyze.py
python3 utilities/zipc-packetrate-analyze.py --quick
```

Execution order is randomized per repetition to reduce ordering and thermal
bias. Results are written below `build/benchmarks/` as raw CSV, summary CSV,
host metadata, and a Markdown report. The analyzer uses GNU `time` for aggregate
CPU time, context switches, faults, and per-process maximum RSS, and samples the
Linux `/proc` process tree for aggregate peak PSS so shared mappings are
apportioned rather than counted once per process. Hardware counters are not
collected by this script. Host metadata records the executable SHA-256, analyzer
Git revision and dirty state, analyzer arguments, build invocation, and ambient
compiler identity. For a custom `--binary`, its source revision must be recorded
separately; the Git fields describe the analyzer checkout. `--quick` is a smoke
test; do not use its short-run resource metrics as performance evidence.

Use `--help` to change payload, packet count, relay sets, repetition count,
fixed slot count, per-run timeout, cooldown, binary, or output location. The
analyzer requires Linux with `smaps_rollup` and GNU `time`. Generated benchmark
results below `build/benchmarks/` are not part of the source manifest. The
selected 2026-08-16 historical evidence referenced below is preserved in the
source manifest under `docs/benchmark-data/`.

The recorded 11200-byte, 0-100 relay experiment and its IPC verdict are in
[`docs/PACKETRATE-BENCHMARK.md`](../docs/PACKETRATE-BENCHMARK.md).


## zipc-stat

Inspect a live POSIX-shared-memory pool created with matching geometry:

```sh
./build/utilities/zipc-stat --name /zipc_pool --slots 64 --capacity 4096
```

Add `--all` to include free slots. The output contains pool counters, component epochs and heartbeats, slot owner/age/hop limits, recovery counts, and the retained trace entries.

The observer supports conventional contiguous pools with derived slot stride
and 64-byte alignment. Explicit-stride, guard-page, and other custom layouts
require future geometry-discovery support.


## Backend terminology

Use the canonical three-role model: payload backend, descriptor backend, and event backend. See `docs/BACKENDS.md`.
