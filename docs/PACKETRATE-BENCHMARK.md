# zIPC packet-rate relay scaling benchmark

## Scope

This document records the Linux host benchmark run performed on 2026-08-16
with `zipc-packetrate` and the general IPC assessment derived from it. It is a
descriptor/ownership throughput test, not a latency benchmark or target
hardware validation.

The tested source revision was `9b27cabe0a4f433fe1860af7ecbd4df5355353d4`
(`Support large packet-rate payloads`). The analysis tool was added immediately
after that revision as `utilities/zipc-packetrate-analyze.py`.

## Host and method

Host configuration:

- Linux 6.11.0-29-generic, x86-64;
- Intel Core Ultra 7 155H;
- 22 online logical CPUs;
- one NUMA node;
- 24 MiB shared L3 cache;
- `kernel.perf_event_paranoid=4`.

Benchmark configuration:

- transport: shared SPSC ring with eventfd notification;
- payload: 11,200 bytes;
- measured packets per run: 1,000,000;
- warm-up packets per run: 1,000;
- shared pool slots: 256, fixed for every primary and control run;
- relay counts: 0, 1, 2, 5, 10, 20, 50, and 100;
- repetitions: three per configuration;
- execution order: randomized independently for each repetition;
- zero-payload controls: 0 and 100 relays;
- resource collection: GNU `time` and sampled process-tree PSS from Linux
  `smaps_rollup`.

Hardware cycles, instructions, cache misses, and CPU migrations were not
collected because unprivileged perf events were prohibited. CPU normalization
includes measured and warm-up packets because GNU `time` observes the complete
process execution.

Rerun the methodology against the current checkout with:

```sh
python3 utilities/zipc-packetrate-analyze.py
```

To rerun against the historical tested source while using the later analyzer:

```sh
git worktree add /tmp/zipc-packetrate-9b27cab 9b27cab
make -C /tmp/zipc-packetrate-9b27cab build/utilities/zipc-packetrate
python3 utilities/zipc-packetrate-analyze.py \
    --binary /tmp/zipc-packetrate-9b27cab/build/utilities/zipc-packetrate \
    --no-build
```

This reruns the same methodology; operating-system state, CPU frequency,
background load, and toolchain differences can still change the measurements.

The analyzer writes raw CSV, summary CSV, host metadata, and a generated
Markdown report below `build/benchmarks/`.

The historical evidence for this run is preserved in:

- `docs/benchmark-data/packetrate-20260816/raw-reduced.csv`;
- `docs/benchmark-data/packetrate-20260816/summary.csv`;
- `docs/benchmark-data/packetrate-20260816/host.json`.

The data-directory README defines which collection-time fields were retained
and which provenance details were reconstructed after the run.

## Throughput results

| Relays | Hops | Median packets/s | Min | Max | Baseline retained | Aggregate transfers/s | Payload MiB/s |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 1 | 1,314,393 | 1,298,648 | 1,360,797 | 100.0% | 1.31M | 14,039.24 |
| 1 | 2 | 1,351,868 | 1,338,746 | 1,355,448 | 102.9% | 2.70M | 14,439.51 |
| 2 | 3 | 1,145,261 | 1,128,959 | 1,250,912 | 87.1% | 3.44M | 12,232.71 |
| 5 | 6 | 1,005,482 | 985,882 | 1,058,435 | 76.5% | 6.03M | 10,739.70 |
| 10 | 11 | 806,711 | 738,047 | 866,112 | 61.4% | 8.87M | 8,616.60 |
| 20 | 21 | 402,574 | 395,219 | 406,261 | 30.6% | 8.45M | 4,299.96 |
| 50 | 51 | 157,259 | 156,770 | 209,207 | 12.0% | 8.02M | 1,679.71 |
| 100 | 101 | 79,636 | 79,144 | 98,979 | 6.1% | 8.04M | 850.61 |

`Aggregate transfers/s` is end-to-end packets/s multiplied by hop count. It
represents total protocol work across all links, not throughput of one link.
Payload throughput counts each completed payload once and must not be multiplied
by hop count because relays do not copy the payload.

## Efficiency and variability

| Relays | Median CPU use | CPU ns/packet | CPU ns/hop | Context switches/packet | Context switches/hop | Median sampled peak PSS MiB | Rate CV |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 172% | 1,318.7 | 1,318.7 | 0.1803 | 0.1803 | 3.2 | 2.44% |
| 1 | 270% | 2,018.0 | 1,009.0 | 0.1515 | 0.0758 | 3.2 | 0.65% |
| 2 | 350% | 3,047.0 | 1,015.7 | 0.2238 | 0.0746 | 3.3 | 5.63% |
| 5 | 625% | 6,103.9 | 1,017.3 | 0.2545 | 0.0424 | 3.4 | 3.69% |
| 10 | 1,077% | 13,766.2 | 1,251.5 | 0.4168 | 0.0379 | 3.7 | 7.97% |
| 20 | 1,471% | 36,653.3 | 1,745.4 | 0.9146 | 0.0436 | 4.0 | 1.40% |
| 50 | 1,608% | 102,437.6 | 2,008.6 | 2.1224 | 0.0416 | 5.3 | 17.28% |
| 100 | 1,684% | 212,907.1 | 2,108.0 | 4.8614 | 0.0481 | 7.3 | 13.17% |

CPU cost is aggregate process-tree CPU time divided by all measured and warm-up
packets or transfers. CPU utilization may exceed 100% because the process tree
uses multiple logical CPUs. PSS apportions shared mappings rather than counting
the complete shared pool once per process. PSS was sampled periodically and may
miss short-lived peaks.

## Individual packet-rate runs

The following values are packets/s grouped by repetition. Configurations were
executed in randomized order within each repetition:

| Relays | Run 1 | Run 2 | Run 3 |
|---:|---:|---:|---:|
| 0 | 1,360,797 | 1,314,393 | 1,298,648 |
| 1 | 1,338,746 | 1,355,448 | 1,351,868 |
| 2 | 1,250,912 | 1,145,261 | 1,128,959 |
| 5 | 1,005,482 | 985,882 | 1,058,435 |
| 10 | 866,112 | 738,047 | 806,711 |
| 20 | 395,219 | 406,261 | 402,574 |
| 50 | 209,207 | 156,770 | 157,259 |
| 100 | 98,979 | 79,636 | 79,144 |

The earlier standalone observation of 104,389 packets/s at 100 relays is
plausible but optimistic relative to the repeated experiment. The repeated
median is 79,636 packets/s, and the high variability at 50 and 100 relays means
a single result should not be used as the sustained-rate claim.

## Zero-payload controls

| Relays | Median packets/s | Min | Max | Aggregate transfers/s | CPU use | CPU ns/hop | Rate CV |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 2,103,289 | 1,953,304 | 2,143,659 | 2.10M | 182% | 889.1 | 4.85% |
| 100 | 77,910 | 77,696 | 85,691 | 7.87M | 1,662% | 2,113.4 | 5.66% |

At zero relays, the 11,200-byte workload retains 62.5% of the zero-payload
packet rate, showing the cost of the producer payload copy. At 100 relays, it
retains 102.2%; that difference is inside run variability, so payload copying is
no longer the bottleneck. Scheduler, notification, and ownership-transfer work
dominates the long chain.

## Objective interpretation

The system reaches a host-wide aggregate ceiling around ten relays. Aggregate
transfer rate peaks at 8.87 million hops/s with ten relays and remains near
8.0-8.5 million hops/s from 20 through 100 relays. Once this ceiling is reached,
end-to-end packet rate falls approximately as:

```text
end-to-end packets/s ~= aggregate transfers/s / hop count
```

This is resource saturation rather than a payload-copy collapse. At 100 relays,
both the 11,200-byte and zero-payload cases have approximately the same packet
rate and CPU cost per hop.

The one-relay result slightly exceeds the zero-relay median. The difference is
small and may reflect pipeline scheduling, CPU frequency, or ordinary run
variation; it is not evidence that adding a relay intrinsically improves IPC.

The output interval at 100 relays is 12.56 microseconds per completed packet.
That is completion spacing, not the end-to-end latency of one packet. This
benchmark does not measure latency distributions.

## General IPC verdict

For this ring/eventfd producer-copy, relay-forward, and sink-release workload,
the result supports describing zIPC as a promising event-driven, zero-copy IPC
prototype. This experiment alone cannot classify it as generally good or bad
against other IPC implementations because it contains no external baseline,
does not measure latency, and does not read or validate payload bytes at the
consumer. It also does not demonstrate production real-time behavior.

| IPC property | Assessment |
|---|---|
| Relay payload copying | Structurally absent in the tested path |
| Payload effect at 100 relays | Not distinguishable from zero-payload control within variation |
| Bounded memory and backpressure | Present with 256 fixed slots |
| Blocking throughput | Measured, but not compared with another IPC implementation |
| CPU efficiency | Measured, but no acceptance threshold or competitor baseline exists |
| Scaling beyond 10-20 processes | Aggregate transfer ceiling reached in this experiment |
| Predictability in very large chains | 13-17% run-to-run CV at 50-100 relays |
| End-to-end and tail latency evidence | Missing |
| Consumer payload integrity/access validation | Missing |

The strongest workload-specific result is that payload cost is not visible at
100 relays: a zero payload and an 11,200-byte payload perform the same within
observed variation. This is consistent with chained zero-copy IPC, but the
controls at only 0 and 100 relays and the lack of sink payload access mean it is
not a complete payload-cost characterization.

For chains of one through ten relays, the measured range is approximately
0.8-1.35 million completed packets/s, with about 1.0-1.3 microseconds of CPU
time per hop. Whether those values are acceptable requires an application
target or an identically configured competitor baseline. An optimized
busy-poll shared-memory transport may reduce per-hop cost by dedicating CPU
capacity continuously, but that comparison was not measured here.

For 50-100 process chains, aggregate transfer capacity remains stable but
end-to-end packet rate and predictability degrade. At 100 relays the process
topology contains 102 zIPC components, reaches about 16.8 CPUs of aggregate
utilization, costs about 2.1 microseconds of CPU time per hop, and shows 13.17%
run-to-run variation. This is useful stress evidence, but no real-time claim
follows without measured tail latency and an explicit jitter bound.

Whether 79,636 packets/s across 101 hops is acceptable depends on the product
requirement. It does not reliably satisfy a 100,000 packets/s requirement on
this setup. The experiment objectively establishes approximately eight million
aggregate ownership transfers/s in the saturated long-chain region; assigning
a qualitative grade requires a stated requirement or comparison target.

## Remaining evidence and optimization priorities

The next measurements needed for a complete IPC claim are:

- per-packet end-to-end latency with p50, p99, and maximum;
- per-hop latency decomposition;
- sink-side payload read and integrity validation;
- cycles, instructions, cache misses, and CPU migrations when perf access is
  available;
- repeated runs with controlled CPU frequency and affinity;
- comparison with Unix sockets, pipes, and representative shared-memory IPC
  implementations under identical topology and payload conditions.

The highest-value implementation experiments are notification coalescing,
descriptor batching, optional polling, and explicit CPU-affinity policies. The
benchmark indicates that long-chain performance is limited by execution and
notification cost rather than payload copying.
