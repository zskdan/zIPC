#!/usr/bin/env python3
"""Run and summarize repeatable zipc-packetrate scaling experiments."""

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
import platform
import random
import re
import shlex
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_RELAYS = "0,1,2,5,10,20,50,100"
DEFAULT_CONTROL_RELAYS = "0,100"
TIME_FORMAT = (
    "ZIPC_TIME user=%U system=%S elapsed=%e cpu=%P max_rss_kb=%M "
    "voluntary_cs=%w involuntary_cs=%c major_faults=%F minor_faults=%R"
)
CONFIG_RE = re.compile(
    r"^config: .*?slots=(?P<slots>\d+)(?: \([^)]*\))? "
    r"warmup=(?P<warmup>\d+)$",
    re.MULTILINE,
)
RESULT_RE = re.compile(
    r"^transport=(?P<transport>\S+) relays=(?P<relays>\d+) "
    r"hops=(?P<hops>\d+) packets=(?P<packets>\d+) "
    r"payload=(?P<payload>\d+) elapsed=(?P<elapsed>[0-9.]+) s$",
    re.MULTILINE,
)
RATE_RE = re.compile(
    r"^rate=(?P<rate>[0-9.]+) packets/s "
    r"transfer-rate=(?P<transfer_rate>[0-9.]+) transfers/s "
    r"payload-throughput=(?P<payload_mib_s>[0-9.]+) MiB/s$",
    re.MULTILINE,
)
RAW_FIELDS = [
    "run",
    "repeat",
    "order",
    "kind",
    "transport",
    "payload",
    "relays",
    "hops",
    "slots",
    "warmup_packets",
    "packets",
    "benchmark_elapsed_s",
    "packets_per_s",
    "transfers_per_s",
    "payload_mib_s",
    "whole_wall_s",
    "time_elapsed_s",
    "user_s",
    "system_s",
    "cpu_s",
    "cpu_utilization_percent",
    "cpu_ns_per_packet",
    "cpu_ns_per_transfer",
    "voluntary_cs",
    "involuntary_cs",
    "context_switches",
    "context_switches_per_packet",
    "context_switches_per_transfer",
    "major_faults",
    "minor_faults",
    "time_max_rss_kb",
    "sampled_peak_tree_pss_kb",
    "sampled_peak_processes",
    "command",
]
SUMMARY_FIELDS = [
    "kind",
    "payload",
    "relays",
    "hops",
    "runs",
    "median_packets_per_s",
    "min_packets_per_s",
    "max_packets_per_s",
    "rate_cv_percent",
    "retention_percent",
    "median_transfers_per_s",
    "median_payload_mib_s",
    "median_cpu_utilization_percent",
    "median_cpu_ns_per_packet",
    "median_cpu_ns_per_transfer",
    "median_context_switches_per_packet",
    "median_context_switches_per_transfer",
    "median_peak_tree_pss_mib",
]


def parse_number_list(text, name):
    values = []
    try:
        for item in text.split(","):
            value = int(item)
            if value < 0 or value > 252:
                raise ValueError
            if value not in values:
                values.append(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            f"{name} must be a comma-separated list in the range 0..252"
        ) from error
    if not values:
        raise argparse.ArgumentTypeError(f"{name} must not be empty")
    return values


def parse_payload_size(text):
    match = re.fullmatch(r"([0-9]+)([KkMmGg]?)", text)
    if match is None:
        raise argparse.ArgumentTypeError("payload must be decimal bytes or K/M/G")
    multiplier = {
        "": 1,
        "k": 1024,
        "m": 1024 * 1024,
        "g": 1024 * 1024 * 1024,
    }[match.group(2).lower()]
    value = int(match.group(1)) * multiplier
    if value > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("payload exceeds the zIPC uint32 limit")
    return value


def automatic_slots(payload_size):
    capacity = max(payload_size, 1)
    stride = (capacity + 63) & ~63
    if stride > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("payload cannot be aligned to 64 bytes")
    return max(2, min(256, (32 * 1024 * 1024) // stride))


def read_text(path):
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError):
        return ""


def process_tree(root_pid):
    pending = [root_pid]
    found = set()
    while pending:
        pid = pending.pop()
        if pid in found:
            continue
        found.add(pid)
        children_path = Path(f"/proc/{pid}/task/{pid}/children")
        text = read_text(children_path).strip()
        if text:
            for child in text.split():
                try:
                    pending.append(int(child))
                except ValueError:
                    pass
    return found


def process_pss_kb(pid):
    for line in read_text(Path(f"/proc/{pid}/smaps_rollup")).splitlines():
        if line.startswith("Pss:"):
            fields = line.split()
            if len(fields) >= 2:
                try:
                    return int(fields[1])
                except ValueError:
                    return None
    return None


def terminate_process_group(process):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2.0)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        pass


def run_timed(command, environment, timeout_s):
    timed_command = ["/usr/bin/time", "-f", TIME_FORMAT] + command
    started = time.perf_counter()
    process = subprocess.Popen(
        timed_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
        start_new_session=True,
    )
    peak_pss_kb = None
    peak_processes = 0
    deadline = started + timeout_s
    try:
        while process.poll() is None:
            if time.perf_counter() >= deadline:
                raise RuntimeError(
                    f"command exceeded {timeout_s:.1f}s: {shlex.join(command)}"
                )
            pids = process_tree(process.pid)
            samples = [process_pss_kb(pid) for pid in pids]
            observed = [value for value in samples if value is not None]
            if observed:
                pss_kb = sum(observed)
                peak_pss_kb = (
                    pss_kb if peak_pss_kb is None else max(peak_pss_kb, pss_kb)
                )
            peak_processes = max(peak_processes, len(pids))
            try:
                process.wait(timeout=0.02)
            except subprocess.TimeoutExpired:
                pass
        stdout, stderr = process.communicate()
    except BaseException:
        terminate_process_group(process)
        process.communicate()
        raise
    whole_wall_s = time.perf_counter() - started

    timing_line = None
    diagnostic_lines = []
    for line in stderr.splitlines():
        if line.startswith("ZIPC_TIME "):
            timing_line = line
        elif line:
            diagnostic_lines.append(line)
    if process.returncode != 0 or timing_line is None:
        details = "\n".join(diagnostic_lines)
        raise RuntimeError(
            f"command failed with status {process.returncode}: "
            f"{shlex.join(command)}\n{stdout}\n{details}"
        )

    timing = {}
    for field in timing_line.split()[1:]:
        key, value = field.split("=", 1)
        timing[key] = value.rstrip("%")
    timing["whole_wall_s"] = whole_wall_s
    timing["peak_tree_pss_kb"] = peak_pss_kb
    timing["peak_processes"] = peak_processes
    return stdout, timing


def parse_benchmark_output(stdout):
    config = CONFIG_RE.search(stdout)
    result = RESULT_RE.search(stdout)
    rate = RATE_RE.search(stdout)
    if config is None or result is None or rate is None:
        raise RuntimeError(f"cannot parse zipc-packetrate output:\n{stdout}")
    parsed = {}
    parsed.update(config.groupdict())
    parsed.update(result.groupdict())
    parsed.update(rate.groupdict())
    return parsed


def make_row(
    run_number,
    repeat,
    order,
    kind,
    command,
    stdout,
    timing,
    expected_payload,
    expected_relays,
    expected_packets,
):
    parsed = parse_benchmark_output(stdout)
    packets = int(parsed["packets"])
    payload = int(parsed["payload"])
    relays = int(parsed["relays"])
    if (
        parsed["transport"] != "ring-eventfd"
        or payload != expected_payload
        or relays != expected_relays
        or packets != expected_packets
    ):
        raise RuntimeError(
            "benchmark output does not match the requested transport, payload, "
            "relay count, or packet count"
        )
    warmup = int(parsed["warmup"])
    hops = int(parsed["hops"])
    total_packets = packets + warmup
    total_transfers = total_packets * hops
    user_s = float(timing["user"])
    system_s = float(timing["system"])
    cpu_s = user_s + system_s
    whole_wall_s = float(timing["whole_wall_s"])
    voluntary_cs = int(timing["voluntary_cs"])
    involuntary_cs = int(timing["involuntary_cs"])
    context_switches = voluntary_cs + involuntary_cs
    return {
        "run": run_number,
        "repeat": repeat,
        "order": order,
        "kind": kind,
        "transport": parsed["transport"],
        "payload": payload,
        "relays": relays,
        "hops": hops,
        "slots": int(parsed["slots"]),
        "warmup_packets": warmup,
        "packets": packets,
        "benchmark_elapsed_s": float(parsed["elapsed"]),
        "packets_per_s": float(parsed["rate"]),
        "transfers_per_s": float(parsed["transfer_rate"]),
        "payload_mib_s": float(parsed["payload_mib_s"]),
        "whole_wall_s": whole_wall_s,
        "time_elapsed_s": float(timing["elapsed"]),
        "user_s": user_s,
        "system_s": system_s,
        "cpu_s": cpu_s,
        "cpu_utilization_percent": 100.0 * cpu_s / whole_wall_s,
        "cpu_ns_per_packet": cpu_s * 1e9 / total_packets,
        "cpu_ns_per_transfer": cpu_s * 1e9 / total_transfers,
        "voluntary_cs": voluntary_cs,
        "involuntary_cs": involuntary_cs,
        "context_switches": context_switches,
        "context_switches_per_packet": context_switches / total_packets,
        "context_switches_per_transfer": context_switches / total_transfers,
        "major_faults": int(timing["major_faults"]),
        "minor_faults": int(timing["minor_faults"]),
        "time_max_rss_kb": int(timing["max_rss_kb"]),
        "sampled_peak_tree_pss_kb": timing["peak_tree_pss_kb"],
        "sampled_peak_processes": int(timing["peak_processes"]),
        "command": shlex.join(command),
    }


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def median(rows, field):
    return statistics.median(float(row[field]) for row in rows)


def median_optional(rows, field):
    values = [float(row[field]) for row in rows if row[field] not in (None, "")]
    return statistics.median(values) if values else math.nan


def summarize(raw_rows):
    groups = {}
    for row in raw_rows:
        key = (row["kind"], row["payload"], row["relays"], row["hops"])
        groups.setdefault(key, []).append(row)

    summaries = []
    for key, rows in groups.items():
        rates = [float(row["packets_per_s"]) for row in rows]
        mean_rate = statistics.mean(rates)
        deviation = statistics.stdev(rates) if len(rates) > 1 else math.nan
        summaries.append(
            {
                "kind": key[0],
                "payload": key[1],
                "relays": key[2],
                "hops": key[3],
                "runs": len(rows),
                "median_packets_per_s": statistics.median(rates),
                "min_packets_per_s": min(rates),
                "max_packets_per_s": max(rates),
                "rate_cv_percent": (
                    100.0 * deviation / mean_rate
                    if not math.isnan(deviation)
                    else math.nan
                ),
                "retention_percent": math.nan,
                "median_transfers_per_s": median(rows, "transfers_per_s"),
                "median_payload_mib_s": median(rows, "payload_mib_s"),
                "median_cpu_utilization_percent": median(
                    rows, "cpu_utilization_percent"
                ),
                "median_cpu_ns_per_packet": median(rows, "cpu_ns_per_packet"),
                "median_cpu_ns_per_transfer": median(
                    rows, "cpu_ns_per_transfer"
                ),
                "median_context_switches_per_packet": median(
                    rows, "context_switches_per_packet"
                ),
                "median_context_switches_per_transfer": median(
                    rows, "context_switches_per_transfer"
                ),
                "median_peak_tree_pss_mib": median_optional(
                    rows, "sampled_peak_tree_pss_kb"
                )
                / 1024.0,
            }
        )

    primary = [row for row in summaries if row["kind"] == "primary"]
    baseline = next((row for row in primary if row["relays"] == 0), None)
    if baseline is not None:
        baseline_rate = baseline["median_packets_per_s"]
        for row in primary:
            row["retention_percent"] = (
                100.0 * row["median_packets_per_s"] / baseline_rate
            )
    summaries.sort(
        key=lambda row: (0 if row["kind"] == "primary" else 1, row["relays"])
    )
    return summaries


def format_float(value, digits=2):
    if isinstance(value, float) and math.isnan(value):
        return "n/a"
    return f"{value:.{digits}f}"


def make_report(arguments, host, summaries):
    primary = [row for row in summaries if row["kind"] == "primary"]
    controls = [row for row in summaries if row["kind"] == "control"]
    lines = [
        "# zIPC packet-rate scaling report",
        "",
        f"Generated: {host['generated_at']}",
        "",
        "## Configuration",
        "",
        f"- Host: `{host['node']}` / `{host['machine']}`",
        f"- Online CPUs: {host['cpu_count']}",
        f"- Payload: `{arguments.payload}`",
        f"- Measured packets per run: {arguments.packets}",
        f"- Repetitions: {arguments.repeats}",
        f"- Relay sweep: `{','.join(str(value) for value in arguments.relay_values)}`",
        f"- Slots: {arguments.selected_slots} (fixed for primary and controls)",
        f"- Binary SHA-256: `{host['binary_sha256']}`",
        f"- Analyzer worktree dirty before build: {'yes' if host['analyzer_source_git_status_porcelain'] else 'no'}",
        f"- Hardware performance counters: not collected (`perf_event_paranoid={host['perf_event_paranoid']}`)",
        "- Resource metrics: GNU `time` plus sampled aggregate `/proc` PSS",
        "",
        "## Relay scaling",
        "",
        "| Relays | Hops | Median pkt/s | Retention | Aggregate Mtransfers/s | MiB/s | CPU use | CPU ns/hop | Ctx/packet | Median sampled peak PSS MiB | CV |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in primary:
        lines.append(
            "| {relays} | {hops} | {rate:,.0f} | {retention}% | "
            "{transfers:.2f} | {mib:.2f} | {cpu:.0f}% | {cpu_hop:.1f} | "
            "{ctx:.4f} | {pss} | {cv} |".format(
                relays=row["relays"],
                hops=row["hops"],
                rate=row["median_packets_per_s"],
                retention=format_float(row["retention_percent"], 1),
                transfers=row["median_transfers_per_s"] / 1e6,
                mib=row["median_payload_mib_s"],
                cpu=row["median_cpu_utilization_percent"],
                cpu_hop=row["median_cpu_ns_per_transfer"],
                ctx=row["median_context_switches_per_packet"],
                pss=format_float(row["median_peak_tree_pss_mib"], 1),
                cv=(
                    "n/a"
                    if math.isnan(row["rate_cv_percent"])
                    else f"{row['rate_cv_percent']:.2f}%"
                ),
            )
        )

    if controls:
        lines.extend(
            [
                "",
                "## Descriptor-only controls",
                "",
                "| Relays | Hops | Median pkt/s | Aggregate Mtransfers/s | CPU use | CPU ns/hop | CV |",
                "|---:|---:|---:|---:|---:|---:|---:|",
            ]
        )
        for row in controls:
            lines.append(
                "| {relays} | {hops} | {rate:,.0f} | {transfers:.2f} | "
                "{cpu:.0f}% | {cpu_hop:.1f} | {cv} |".format(
                    relays=row["relays"],
                    hops=row["hops"],
                    rate=row["median_packets_per_s"],
                    transfers=row["median_transfers_per_s"] / 1e6,
                    cpu=row["median_cpu_utilization_percent"],
                    cpu_hop=row["median_cpu_ns_per_transfer"],
                    cv=(
                        "n/a"
                        if math.isnan(row["rate_cv_percent"])
                        else f"{row['rate_cv_percent']:.2f}%"
                    ),
                )
            )

    lines.extend(["", "## Automatic observations", ""])
    if arguments.quick:
        lines.append(
            "- Quick mode is a smoke test; its short-run CPU, PSS, and "
            "variability values are not suitable for performance conclusions."
        )
    if primary and not arguments.quick:
        baseline = next((row for row in primary if row["relays"] == 0), None)
        largest = max(primary, key=lambda row: row["relays"])
        if baseline is not None:
            lines.append(
                f"- Zero-relay baseline: {baseline['median_packets_per_s']:,.0f} packets/s."
            )
            lines.append(
                f"- At {largest['relays']} relays, throughput is "
                f"{largest['median_packets_per_s']:,.0f} packets/s "
                f"({largest['retention_percent']:.1f}% of baseline)."
            )
        lines.append(
            f"- The largest chain completes one packet every "
            f"{1e6 / largest['median_packets_per_s']:.2f} microseconds at the output; "
            "this is completion spacing, not end-to-end latency."
        )
        lines.append(
            f"- Its aggregate protocol rate is "
            f"{largest['median_transfers_per_s'] / 1e6:.2f} million hop transfers/s."
        )
        lines.append(
            f"- Median whole-tree CPU cost is "
            f"{largest['median_cpu_ns_per_transfer']:.1f} ns per processed hop "
            "when warm-up packets are included."
        )
        measured_cv = [
            row for row in primary if not math.isnan(row["rate_cv_percent"])
        ]
        if measured_cv:
            worst_cv = max(measured_cv, key=lambda row: row["rate_cv_percent"])
            lines.append(
                f"- Maximum run-to-run rate variation is "
                f"{worst_cv['rate_cv_percent']:.2f}% CV at "
                f"{worst_cv['relays']} relays."
            )
        below_90 = next(
            (row for row in primary if row["retention_percent"] < 90.0), None
        )
        if below_90 is not None:
            lines.append(
                f"- Throughput first falls below 90% of baseline at "
                f"{below_90['relays']} relays."
            )

    for control in controls if not arguments.quick else []:
        matching = next(
            (row for row in primary if row["relays"] == control["relays"]), None
        )
        if matching is not None:
            ratio = (
                100.0
                * matching["median_packets_per_s"]
                / control["median_packets_per_s"]
            )
            lines.append(
                f"- At {control['relays']} relays, the configured payload retains "
                f"{ratio:.1f}% of the zero-payload packet rate."
            )

    lines.extend(
        [
            "",
            "## Interpretation limits",
            "",
            "- `transfers/s` is packets/s multiplied by hop count; it is aggregate work, not one-link throughput.",
            "- Payload throughput counts each completed payload once. Relays do not copy or read all payload bytes.",
            "- Completion spacing is not end-to-end latency; this utility does not timestamp individual packets.",
            "- GNU `time` includes process setup and warm-up. CPU normalization includes measured and warm-up packets to reduce that bias.",
            "- PSS is sampled from the process tree to apportion shared mappings and may miss very short-lived peaks.",
            "- Cycles, instructions, cache misses, and CPU migrations were not collected.",
            "",
        ]
    )
    return "\n".join(lines)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_host_metadata(
    root, binary, arguments, source_git_status, build_invoked, build_command
):
    generated_at = dt.datetime.now(dt.timezone.utc).isoformat()
    metadata = {
        "generated_at": generated_at,
        "node": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count() or 0,
        "perf_event_paranoid": read_text(
            Path("/proc/sys/kernel/perf_event_paranoid")
        ).strip(),
        "binary": str(binary),
        "binary_sha256": sha256_file(binary),
        "analyzer_argv": sys.argv,
        "analyzer_source_git_status_porcelain": source_git_status,
        "build_invoked": build_invoked,
        "build_command": build_command,
        "build_environment": {
            name: os.environ.get(name) for name in ("CC", "CFLAGS", "CPPFLAGS", "LDLIBS")
        },
        "experiment": {
            "payload": arguments.payload,
            "payload_bytes": arguments.payload_bytes,
            "packets": arguments.packets,
            "slots": arguments.selected_slots,
            "relays": arguments.relay_values,
            "control_relays": arguments.control_relay_values,
            "repeats": arguments.repeats,
            "cooldown": arguments.cooldown,
            "timeout": arguments.timeout,
            "seed": arguments.seed,
            "quick": arguments.quick,
        },
    }
    try:
        metadata["analyzer_git_commit"] = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        metadata["analyzer_git_commit"] = "unknown"
    try:
        metadata["ambient_compiler"] = subprocess.run(
            [os.environ.get("CC", "cc"), "--version"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        metadata["ambient_compiler"] = "unavailable"
    try:
        metadata["lscpu"] = subprocess.run(
            ["lscpu"], check=True, capture_output=True, text=True
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        metadata["lscpu"] = "unavailable"
    return metadata


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, help="zipc-packetrate binary")
    parser.add_argument("--payload", default="11200", help="primary payload size")
    parser.add_argument("--packets", type=int, default=1_000_000)
    parser.add_argument("--slots", type=int, help="fixed slot count for all runs")
    parser.add_argument("--relays", default=DEFAULT_RELAYS)
    parser.add_argument("--control-relays", default=DEFAULT_CONTROL_RELAYS)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--cooldown", type=float, default=0.25)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--seed", type=int, default=20260816)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--no-control", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument(
        "--quick",
        action="store_true",
        help="one 100,000-packet smoke run at zero and two relays",
    )
    arguments = parser.parse_args()
    if (
        arguments.packets <= 0
        or arguments.repeats <= 0
        or arguments.cooldown < 0
        or arguments.timeout <= 0
        or (
            arguments.slots is not None
            and (arguments.slots < 2 or arguments.slots > 0xFFFFFFFF)
        )
    ):
        parser.error(
            "packets/repeats/timeout must be positive, cooldown nonnegative, "
            "and slots at least two"
        )
    try:
        arguments.payload_bytes = parse_payload_size(arguments.payload)
        arguments.relay_values = parse_number_list(arguments.relays, "relays")
        arguments.control_relay_values = parse_number_list(
            arguments.control_relays, "control relays"
        )
    except argparse.ArgumentTypeError as error:
        parser.error(str(error))
    arguments.selected_slots = (
        arguments.slots
        if arguments.slots is not None
        else automatic_slots(arguments.payload_bytes)
    )
    if arguments.quick:
        arguments.packets = 100_000
        arguments.repeats = 1
        arguments.relay_values = [0, 2]
        arguments.control_relay_values = []
        arguments.no_control = True
        arguments.cooldown = 0.0
    return arguments


def main():
    arguments = parse_arguments()
    root = Path(__file__).resolve().parents[1]
    binary = arguments.binary or root / "build/utilities/zipc-packetrate"
    binary = binary.resolve()
    try:
        source_git_status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        source_git_status = "\n".join(
            line
            for line in source_git_status.splitlines()
            if not line.startswith("?? build/")
        )
    except (OSError, subprocess.CalledProcessError):
        source_git_status = "unavailable"
    build_invoked = not arguments.no_build and arguments.binary is None
    build_command = ["make", "build/utilities/zipc-packetrate"]
    if build_invoked:
        subprocess.run(build_command, cwd=root, check=True)
    if not binary.is_file():
        raise SystemExit(f"benchmark binary not found: {binary}")
    if not Path("/usr/bin/time").is_file():
        raise SystemExit("GNU /usr/bin/time is required")
    if sys.platform != "linux" or not Path("/proc/self/smaps_rollup").is_file():
        raise SystemExit("the analyzer requires Linux /proc with smaps_rollup")
    time_version = subprocess.run(
        ["/usr/bin/time", "--version"], capture_output=True, text=True, check=False
    )
    if "GNU Time" not in time_version.stdout + time_version.stderr:
        raise SystemExit("/usr/bin/time is not GNU time")

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = arguments.output or root / f"build/benchmarks/packetrate-{timestamp}"
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)

    host = collect_host_metadata(
        root,
        binary,
        arguments,
        source_git_status,
        build_invoked,
        build_command if build_invoked else None,
    )
    (output / "host.json").write_text(
        json.dumps(host, indent=2) + "\n", encoding="utf-8"
    )
    (output / "lscpu.txt").write_text(host["lscpu"], encoding="utf-8")

    configurations = [
        {"kind": "primary", "payload": arguments.payload, "relays": relay}
        for relay in arguments.relay_values
    ]
    if not arguments.no_control:
        configurations.extend(
            {"kind": "control", "payload": "0", "relays": relay}
            for relay in arguments.control_relay_values
        )

    environment = dict(os.environ)
    environment["LC_ALL"] = "C"
    raw_rows = []
    run_number = 0
    total_runs = len(configurations) * arguments.repeats
    for repeat in range(1, arguments.repeats + 1):
        ordered = list(configurations)
        random.Random(arguments.seed + repeat).shuffle(ordered)
        for order, configuration in enumerate(ordered, 1):
            run_number += 1
            command = [
                str(binary),
                "--transport",
                "ring-eventfd",
                "--packets",
                str(arguments.packets),
                "--payload",
                configuration["payload"],
                "--relays",
                str(configuration["relays"]),
                "--slots",
                str(arguments.selected_slots),
            ]
            print(
                f"[{run_number}/{total_runs}] repeat={repeat} "
                f"kind={configuration['kind']} payload={configuration['payload']} "
                f"relays={configuration['relays']}",
                flush=True,
            )
            stdout, timing = run_timed(command, environment, arguments.timeout)
            row = make_row(
                run_number,
                repeat,
                order,
                configuration["kind"],
                command,
                stdout,
                timing,
                (
                    arguments.payload_bytes
                    if configuration["kind"] == "primary"
                    else 0
                ),
                configuration["relays"],
                arguments.packets,
            )
            raw_rows.append(row)
            write_csv(output / "raw.csv", RAW_FIELDS, raw_rows)
            print(
                f"  {row['packets_per_s']:,.0f} packets/s, "
                f"{row['cpu_utilization_percent']:.0f}% CPU, "
                f"{row['cpu_ns_per_transfer']:.1f} CPU ns/hop",
                flush=True,
            )
            if arguments.cooldown > 0 and run_number < total_runs:
                time.sleep(arguments.cooldown)

    summaries = summarize(raw_rows)
    write_csv(output / "summary.csv", SUMMARY_FIELDS, summaries)
    report = make_report(arguments, host, summaries)
    (output / "report.md").write_text(report, encoding="utf-8")
    print("\n" + report)
    print(f"Results: {output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        sys.exit(130)
    except (
        KeyError,
        OSError,
        RuntimeError,
        ValueError,
        ZeroDivisionError,
        subprocess.CalledProcessError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
