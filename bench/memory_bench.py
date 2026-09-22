#!/usr/bin/env python3
"""Compare GC-off and GC-on peak RSS and execution time.

The runner uses wait4(2), the same per-process resource accounting used by
/usr/bin/time, and normalizes Darwin's byte-valued ru_maxrss and Linux's
KiB-valued ru_maxrss to KiB.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import os
from pathlib import Path
import shlex
import statistics
import struct
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple


@dataclass
class RunResult:
    sequence: int
    mode: str
    run: int
    exit_code: int
    elapsed_seconds: float
    cpu_user_seconds: float
    cpu_system_seconds: float
    peak_rss_kib: int
    log_path: str
    command: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run matched GC-off/GC-on commands and record peak RSS."
    )
    parser.add_argument("--binary", help="Contrees benchmark executable")
    parser.add_argument("--dataset", help="binary YCSB-C workload")
    parser.add_argument("--scheduler", default="seqcow")
    parser.add_argument("--structure", default="art")
    parser.add_argument("--clients", type=int, default=1)
    parser.add_argument("--pipes", type=int, default=1)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument(
        "--off-command",
        help="complete GC-off command (generic mode; quote as one argument)",
    )
    parser.add_argument(
        "--on-command",
        help="complete GC-on command (generic mode; quote as one argument)",
    )
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=0)
    parser.add_argument("--output", help="CSV path; default is timestamped")
    parser.add_argument("--logs-dir", help="directory for per-run program logs")
    parser.add_argument(
        "extra_args",
        nargs=argparse.REMAINDER,
        help="extra benchmark arguments after -- (structured mode only)",
    )
    return parser.parse_args()


def fail(message: str) -> None:
    raise SystemExit(f"memory_bench.py: {message}")


def build_commands(args: argparse.Namespace) -> Tuple[Dict[str, List[str]], Optional[Path]]:
    generic_mode = args.off_command is not None or args.on_command is not None
    if generic_mode:
        if args.off_command is None or args.on_command is None:
            fail("generic mode requires both --off-command and --on-command")
        if args.extra_args:
            fail("arguments after -- are only valid with --binary/--dataset")
        off = shlex.split(args.off_command)
        on = shlex.split(args.on_command)
        if not off or not on:
            fail("GC commands must not be empty")
        return {"gc_off": off, "gc_on": on}, None

    if not args.binary or not args.dataset:
        fail("structured mode requires --binary and --dataset")
    if args.clients <= 0 or args.pipes <= 0 or args.workers <= 0:
        fail("client, pipe, and worker counts must be positive")

    dataset = Path(args.dataset)
    if not dataset.is_file():
        fail(f"dataset does not exist: {dataset}")

    extra = list(args.extra_args)
    if extra and extra[0] == "--":
        extra = extra[1:]
    base = [
        args.binary,
        str(dataset),
        args.scheduler,
        args.structure,
        "-c",
        str(args.clients),
        "-p",
        str(args.pipes),
        "-w",
        str(args.workers),
    ] + extra
    return {"gc_off": base, "gc_on": base + ["-g"]}, dataset


def peak_rss_kib(raw_maxrss: int) -> int:
    # Darwin exposes bytes; Linux exposes KiB. The project targets these two
    # platforms, and this mirrors memory_stats.hpp.
    if sys.platform == "darwin":
        return (raw_maxrss + 1023) // 1024
    return raw_maxrss


def wait_for_child(pid: int):
    while True:
        try:
            return os.wait4(pid, 0)
        except InterruptedError:
            continue


def decode_status(status: int) -> int:
    if os.WIFEXITED(status):
        return os.WEXITSTATUS(status)
    if os.WIFSIGNALED(status):
        return 128 + os.WTERMSIG(status)
    return 255


def run_command(
    command: Sequence[str], log_path: Path, mode: str, run: int, sequence: int
) -> RunResult:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    pid = os.fork()
    if pid == 0:
        try:
            fd = os.open(
                log_path,
                os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                0o644,
            )
            os.dup2(fd, 1)
            os.dup2(fd, 2)
            if fd > 2:
                os.close(fd)
            os.execvpe(command[0], list(command), os.environ.copy())
        except BaseException as exc:  # Only runs in the child before exec.
            message = f"failed to execute {command[0]!r}: {exc}\n"
            os.write(2, message.encode("utf-8", errors="replace"))
            os._exit(127)

    _, status, usage = wait_for_child(pid)
    elapsed = time.monotonic() - started
    return RunResult(
        sequence=sequence,
        mode=mode,
        run=run,
        exit_code=decode_status(status),
        elapsed_seconds=elapsed,
        cpu_user_seconds=usage.ru_utime,
        cpu_system_seconds=usage.ru_stime,
        peak_rss_kib=peak_rss_kib(usage.ru_maxrss),
        log_path=str(log_path),
        command=shlex.join(command),
    )


def workload_metadata(dataset: Optional[Path]) -> Optional[Tuple[int, int, int]]:
    if dataset is None:
        return None
    size_t_bytes = struct.calcsize("N")
    try:
        with dataset.open("rb") as stream:
            header = stream.read(size_t_bytes * 2)
        if len(header) != size_t_bytes * 2:
            return None
        records, transactions = struct.unpack("@NN", header)
    except (OSError, struct.error):
        return None

    # Expected x86-64 layout in src/main.cpp:
    # elems: 8*n, kvs: 16*n, tx_context: 24*m.
    input_bytes = 24 * records + 24 * transactions
    return records, transactions, input_bytes


def write_csv(
    output: Path,
    results: Sequence[RunResult],
    metadata: Optional[Tuple[int, int, int]],
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    records = metadata[0] if metadata else ""
    transactions = metadata[1] if metadata else ""
    estimated_input_kib = metadata[2] / 1024.0 if metadata else ""
    fieldnames = [
        "sequence",
        "mode",
        "run",
        "exit_code",
        "elapsed_seconds",
        "cpu_user_seconds",
        "cpu_system_seconds",
        "peak_rss_kib",
        "num_records",
        "num_transactions",
        "estimated_input_kib",
        "log_path",
        "command",
    ]
    with output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        for result in results:
            writer.writerow(
                {
                    "sequence": result.sequence,
                    "mode": result.mode,
                    "run": result.run,
                    "exit_code": result.exit_code,
                    "elapsed_seconds": f"{result.elapsed_seconds:.9f}",
                    "cpu_user_seconds": f"{result.cpu_user_seconds:.9f}",
                    "cpu_system_seconds": f"{result.cpu_system_seconds:.9f}",
                    "peak_rss_kib": result.peak_rss_kib,
                    "num_records": records,
                    "num_transactions": transactions,
                    "estimated_input_kib": (
                        f"{estimated_input_kib:.3f}"
                        if isinstance(estimated_input_kib, float)
                        else ""
                    ),
                    "log_path": result.log_path,
                    "command": result.command,
                }
            )


def mean(values: Sequence[float]) -> float:
    return statistics.fmean(values) if values else float("nan")


def print_summary(results: Sequence[RunResult]) -> None:
    print(
        "\nmode     ok/runs  elapsed mean(s)  CPU mean(s)  "
        "peak RSS mean(KiB)  peak RSS max(KiB)"
    )
    print(
        "-------- -------- ---------------- ------------ "
        "------------------- -----------------"
    )
    grouped: Dict[str, List[RunResult]] = {"gc_off": [], "gc_on": []}
    for result in results:
        grouped[result.mode].append(result)

    successful: Dict[str, List[RunResult]] = {}
    for mode in ("gc_off", "gc_on"):
        rows = grouped[mode]
        ok = [row for row in rows if row.exit_code == 0]
        successful[mode] = ok
        elapsed = mean([row.elapsed_seconds for row in ok])
        cpu = mean(
            [row.cpu_user_seconds + row.cpu_system_seconds for row in ok]
        )
        rss = mean([row.peak_rss_kib for row in ok])
        rss_max = max((row.peak_rss_kib for row in ok), default=0)
        print(
            f"{mode:<8} {len(ok):>2}/{len(rows):<5} "
            f"{elapsed:>16.6f} {cpu:>12.6f} "
            f"{rss:>19.1f} {rss_max:>17d}"
        )

    off = successful["gc_off"]
    on = successful["gc_on"]
    if off and on:
        off_rss = mean([row.peak_rss_kib for row in off])
        on_rss = mean([row.peak_rss_kib for row in on])
        off_time = mean([row.elapsed_seconds for row in off])
        on_time = mean([row.elapsed_seconds for row in on])
        off_cpu = mean(
            [row.cpu_user_seconds + row.cpu_system_seconds for row in off]
        )
        on_cpu = mean(
            [row.cpu_user_seconds + row.cpu_system_seconds for row in on]
        )
        rss_change = (on_rss / off_rss - 1.0) * 100.0 if off_rss else float("nan")
        time_change = (
            (on_time / off_time - 1.0) * 100.0 if off_time else float("nan")
        )
        cpu_change = (
            (on_cpu / off_cpu - 1.0) * 100.0 if off_cpu else float("nan")
        )
        print(f"\nGC-on mean peak-RSS change: {rss_change:+.2f}%")
        print(f"GC-on mean elapsed-time change: {time_change:+.2f}%")
        print(f"GC-on mean CPU-time change: {cpu_change:+.2f}%")


def main() -> int:
    args = parse_args()
    if args.runs <= 0 or args.warmups < 0:
        fail("--runs must be positive and --warmups must be non-negative")
    commands, dataset = build_commands(args)

    timestamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = (
        Path(args.output)
        if args.output
        else Path("bench/results") / f"gc_memory_{timestamp}.csv"
    )
    logs_dir = (
        Path(args.logs_dir)
        if args.logs_dir
        else output.parent / f"{output.stem}.logs"
    )

    metadata = workload_metadata(dataset)
    if metadata:
        records, transactions, input_bytes = metadata
        print(
            f"workload: {records} records, {transactions} transactions; "
            f"estimated always-resident input arrays: {input_bytes / (1024**2):.2f} MiB"
        )
    print("peak RSS includes input arrays, the tree, scheduler, threads, and allocator caches")

    for warmup in range(1, args.warmups + 1):
        for mode in ("gc_off", "gc_on"):
            log = logs_dir / f"{mode}_warmup{warmup:02d}.log"
            print(f"warmup {warmup}: {mode}: {shlex.join(commands[mode])}")
            result = run_command(commands[mode], log, mode, 0, 0)
            if result.exit_code != 0:
                print(f"warmup failed (exit {result.exit_code}); see {log}", file=sys.stderr)
                return 1

    results: List[RunResult] = []
    sequence = 0
    for run in range(1, args.runs + 1):
        order = ("gc_off", "gc_on") if run % 2 else ("gc_on", "gc_off")
        for mode in order:
            sequence += 1
            log = logs_dir / f"{mode}_run{run:02d}.log"
            print(f"run {run}/{args.runs}: {mode}: {shlex.join(commands[mode])}")
            result = run_command(commands[mode], log, mode, run, sequence)
            results.append(result)
            print(
                f"  exit={result.exit_code}, elapsed={result.elapsed_seconds:.3f}s, "
                f"peak_rss={result.peak_rss_kib / 1024.0:.2f} MiB"
            )

    write_csv(output, results, metadata)
    print_summary(results)
    print(f"\nCSV: {output}")
    print(f"logs: {logs_dir}")

    failures = [result for result in results if result.exit_code != 0]
    if failures:
        print("one or more benchmark commands failed; inspect the per-run logs", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
