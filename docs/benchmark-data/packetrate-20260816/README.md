# Packet-rate evidence: 2026-08-16

This directory preserves the selected evidence used by
`docs/PACKETRATE-BENCHMARK.md`.

- `summary.csv` is the analyzer summary generated from the original full raw
  output.
- `raw-reduced.csv` is an archival projection of all 30 runs containing the
  configuration, throughput, CPU, context-switch, PSS, and process-count fields
  used in the tracked report. Numeric presentation is shortened from the
  original Python CSV representation but retains the measured precision needed
  for every displayed report value.
- `host.json` preserves collection-time host facts and explicitly labels binary
  hash/compiler details reconstructed immediately afterward.

The original full raw artifact additionally contained GNU `time` user/system
breakdowns, page faults, per-process maximum RSS, command strings, and
high-precision wall-clock bookkeeping. Those fields were not used for the
tracked conclusions and were not retained. Therefore `summary.csv` is not
claimed to be bit-for-bit regenerable from `raw-reduced.csv`.
