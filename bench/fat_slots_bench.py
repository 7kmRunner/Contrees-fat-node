#!/usr/bin/env python3
"""Run a counterbalanced 0/2/4/8 fat-slot benchmark matrix."""

from __future__ import annotations

import argparse
import csv
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


SLOTS: Tuple[int, ...] = (0, 2, 4, 8)

# A four-treatment Williams design.  Across four rounds every slot count occurs
# once in every execution position, and every ordered neighbouring pair occurs
# once.  This avoids always rewarding the last (warmest-cache) treatment.
COUNTERBALANCED_ORDERS: Tuple[Tuple[int, ...], ...] = (
    (0, 2, 8, 4),
    (2, 4, 0, 8),
    (4, 8, 2, 0),
    (8, 0, 4, 2),
)

INTEGER_FIELDS: Tuple[str, ...] = (
    "fat_slots",
    "gc_enabled",
    "clients",
    "records",
    "transactions",
    "reads",
    "scans",
    "updates",
    "ignored_transactions",
    "checksum",
    "gc_retired_nodes",
    "gc_reclaimed_nodes",
    "gc_pending_nodes",
    "gc_retired_bytes",
    "gc_reclaimed_bytes",
    "gc_pending_bytes",
    "live_nodes",
    "live_internal_nodes",
    "live_leaf_nodes",
    "live_sidecar_leaves",
    "live_delta_records",
    "live_requested_bytes",
    "live_sidecar_requested_bytes",
    "current_rss_bytes",
    "peak_rss_bytes",
)

FLOAT_FIELDS: Tuple[str, ...] = (
    "init_elapsed_seconds",
    "elapsed_seconds",
    "gc_collect_elapsed_seconds",
    "total_elapsed_seconds",
)

DRIVER_FIELDS: Tuple[str, ...] = INTEGER_FIELDS + FLOAT_FIELDS
KEY_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MIB = 1024.0 * 1024.0


class BenchmarkError(RuntimeError):
    """A benchmark invocation or its machine-readable output was invalid."""


def positive_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if value <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return value


def nonnegative_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if value < 0:
        raise argparse.ArgumentTypeError("must be zero or greater")
    return value


def parse_arguments(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a compatible fat-node driver in independent processes for "
            "fat-slot "
            "capacities 0, 2, 4, and 8."
        ),
        epilog=(
            "Slot order is counterbalanced between rounds. Warm-up results are "
            "discarded; measured results are written to CSV."
        ),
    )
    parser.add_argument(
        "binary", help="path to a compatible fat-node benchmark binary"
    )
    parser.add_argument("datasets", nargs="+", help="one or more workload files")
    parser.add_argument("--runs", type=positive_int, default=5)
    parser.add_argument("--warmups", type=nonnegative_int, default=1)
    parser.add_argument("--clients", type=positive_int, default=1)
    parser.add_argument(
        "--pipes",
        type=positive_int,
        default=None,
        help="optional pipe count forwarded to drivers that support --pipes",
    )
    parser.add_argument("--gc", action="store_true", help="enable reclamation")
    parser.add_argument(
        "--output", required=True, help="CSV file for measured (non-warm-up) runs"
    )
    return parser.parse_args(argv)


def resolve_binary(text: str) -> Path:
    candidate = Path(text).expanduser()
    if candidate.is_file():
        result = candidate.resolve()
    else:
        located = shutil.which(text)
        if located is None:
            raise BenchmarkError(f"benchmark binary does not exist: {text}")
        result = Path(located).resolve()
    if not os.access(result, os.X_OK):
        raise BenchmarkError(f"benchmark binary is not executable: {result}")
    return result


def resolve_datasets(values: Iterable[str]) -> List[Path]:
    result: List[Path] = []
    seen = set()
    for text in values:
        path = Path(text).expanduser().resolve()
        if not path.is_file():
            raise BenchmarkError(f"dataset does not exist or is not a file: {path}")
        if path in seen:
            raise BenchmarkError(f"dataset was specified more than once: {path}")
        seen.add(path)
        result.append(path)
    return result


def parse_driver_output(output: str) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for line_number, raw_line in enumerate(output.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        if "=" not in line:
            raise BenchmarkError(
                f"driver stdout line {line_number} is not key=value: {raw_line!r}"
            )
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not KEY_PATTERN.fullmatch(key) or not value:
            raise BenchmarkError(
                f"invalid driver stdout line {line_number}: {raw_line!r}"
            )
        if key in fields:
            raise BenchmarkError(f"driver emitted duplicate field: {key}")
        fields[key] = value

    missing = sorted(set(DRIVER_FIELDS) - fields.keys())
    if missing:
        raise BenchmarkError(
            "driver output is missing required field(s): " + ", ".join(missing)
        )

    for key in INTEGER_FIELDS:
        try:
            value = int(fields[key], 10)
        except ValueError as error:
            raise BenchmarkError(
                f"driver field {key} is not an integer: {fields[key]!r}"
            ) from error
        if value < 0:
            raise BenchmarkError(f"driver field {key} is negative: {value}")

    for key in FLOAT_FIELDS:
        try:
            value = float(fields[key])
        except ValueError as error:
            raise BenchmarkError(
                f"driver field {key} is not a number: {fields[key]!r}"
            ) from error
        if not math.isfinite(value) or value < 0.0:
            raise BenchmarkError(
                f"driver field {key} is not a finite nonnegative number: {value}"
            )
    return fields


def invocation_error(
    command: Sequence[str], result: subprocess.CompletedProcess[str]
) -> BenchmarkError:
    rendered = " ".join(repr(part) for part in command)
    return BenchmarkError(
        "benchmark process failed\n"
        f"command: {rendered}\n"
        f"exit code: {result.returncode}\n"
        f"stdout:\n{result.stdout or '<empty>'}\n"
        f"stderr:\n{result.stderr or '<empty>'}"
    )


def run_once(
    binary: Path,
    dataset: Path,
    slots: int,
    clients: int,
    pipes: Optional[int],
    gc_enabled: bool,
) -> Tuple[Dict[str, str], float]:
    command = [
        str(binary),
        str(dataset),
        "--fat-slots",
        str(slots),
        "--clients",
        str(clients),
    ]
    if pipes is not None:
        command.extend(("--pipes", str(pipes)))
    if gc_enabled:
        command.append("--gc")

    started = time.perf_counter()
    try:
        result = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except OSError as error:
        raise BenchmarkError(
            f"could not start benchmark process {str(binary)!r}: {error}"
        ) from error
    wall_seconds = time.perf_counter() - started

    if result.returncode != 0:
        raise invocation_error(command, result)
    try:
        fields = parse_driver_output(result.stdout)
    except BenchmarkError as error:
        rendered = " ".join(repr(part) for part in command)
        raise BenchmarkError(
            f"invalid output from command: {rendered}\n"
            f"{error}\n"
            f"stdout:\n{result.stdout or '<empty>'}\n"
            f"stderr:\n{result.stderr or '<empty>'}"
        ) from error

    if int(fields["fat_slots"]) != slots:
        raise BenchmarkError(
            f"driver reported fat_slots={fields['fat_slots']}, requested {slots}"
        )
    if int(fields["clients"]) != clients:
        raise BenchmarkError(
            f"driver reported clients={fields['clients']}, requested {clients}"
        )
    expected_gc = int(gc_enabled)
    if int(fields["gc_enabled"]) != expected_gc:
        raise BenchmarkError(
            f"driver reported gc_enabled={fields['gc_enabled']}, "
            f"requested {expected_gc}"
        )
    if gc_enabled:
        pending_nodes = int(fields["gc_pending_nodes"])
        pending_bytes = int(fields["gc_pending_bytes"])
        retired_nodes = int(fields["gc_retired_nodes"])
        reclaimed_nodes = int(fields["gc_reclaimed_nodes"])
        retired_bytes = int(fields["gc_retired_bytes"])
        reclaimed_bytes = int(fields["gc_reclaimed_bytes"])
        if pending_nodes != 0 or pending_bytes != 0:
            raise BenchmarkError(
                "GC still has pending objects after final collection: "
                f"nodes={pending_nodes}, bytes={pending_bytes}"
            )
        if retired_nodes != reclaimed_nodes or retired_bytes != reclaimed_bytes:
            raise BenchmarkError(
                "GC retired/reclaimed totals differ after final collection: "
                f"nodes={retired_nodes}/{reclaimed_nodes}, "
                f"bytes={retired_bytes}/{reclaimed_bytes}"
            )
    return fields, wall_seconds


def order_for(round_index: int, dataset_index: int) -> Tuple[int, ...]:
    return COUNTERBALANCED_ORDERS[
        (round_index + dataset_index) % len(COUNTERBALANCED_ORDERS)
    ]


def run_warmups(
    binary: Path,
    datasets: Sequence[Path],
    warmups: int,
    clients: int,
    pipes: Optional[int],
    gc_enabled: bool,
) -> None:
    for warmup_index in range(warmups):
        for dataset_index, dataset in enumerate(datasets):
            order = order_for(warmup_index, dataset_index)
            for position, slots in enumerate(order, start=1):
                print(
                    f"[warmup {warmup_index + 1}/{warmups}] "
                    f"dataset={dataset} order={position}/4 slots={slots}",
                    file=sys.stderr,
                    flush=True,
                )
                run_once(binary, dataset, slots, clients, pipes, gc_enabled)


def run_measurements(
    binary: Path,
    datasets: Sequence[Path],
    runs: int,
    warmups: int,
    clients: int,
    pipes: Optional[int],
    gc_enabled: bool,
) -> List[Dict[str, str]]:
    records: List[Dict[str, str]] = []
    for run_index in range(runs):
        # Rotate dataset order as well, so a multi-dataset run does not always
        # give the same workload the coldest system state.
        dataset_indices = list(range(len(datasets)))
        if dataset_indices:
            offset = run_index % len(dataset_indices)
            dataset_indices = dataset_indices[offset:] + dataset_indices[:offset]

        for dataset_index in dataset_indices:
            dataset = datasets[dataset_index]
            order = order_for(warmups + run_index, dataset_index)
            for position, slots in enumerate(order, start=1):
                print(
                    f"[run {run_index + 1}/{runs}] dataset={dataset} "
                    f"order={position}/4 slots={slots}",
                    file=sys.stderr,
                    flush=True,
                )
                fields, wall_seconds = run_once(
                    binary, dataset, slots, clients, pipes, gc_enabled
                )
                row = {
                    "dataset": str(dataset),
                    "run": str(run_index + 1),
                    "order": str(position),
                    "slots": str(slots),
                    "runner_wall_seconds": f"{wall_seconds:.9f}",
                }
                row.update(fields)
                records.append(row)
    return records


def ordered_driver_fields(records: Sequence[Mapping[str, str]]) -> List[str]:
    observed = set()
    for record in records:
        observed.update(record.keys())
    metadata = {"dataset", "run", "order", "slots", "runner_wall_seconds"}
    expected = [field for field in DRIVER_FIELDS if field in observed]
    extras = sorted(observed - metadata - set(expected))
    return expected + extras


def write_csv(path: Path, records: Sequence[Mapping[str, str]]) -> None:
    path = path.expanduser().resolve()
    columns = ["dataset", "run", "order", "slots", "runner_wall_seconds"]
    columns.extend(ordered_driver_fields(records))

    temporary_name = ""
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_name = output.name
            writer = csv.DictWriter(output, fieldnames=columns, extrasaction="raise")
            writer.writeheader()
            writer.writerows(records)
        os.replace(temporary_name, path)
    except (OSError, csv.Error, ValueError) as error:
        if temporary_name:
            try:
                Path(temporary_name).unlink()
            except OSError:
                pass
        raise BenchmarkError(f"could not write CSV {path}: {error}") from error


def mean_stdev(values: Sequence[float]) -> Tuple[float, float]:
    mean = statistics.fmean(values)
    deviation = statistics.stdev(values) if len(values) > 1 else 0.0
    return mean, deviation


def pair(values: Sequence[float], scale: float, precision: int) -> str:
    mean, deviation = mean_stdev(values)
    return f"{mean / scale:.{precision}f} +/- {deviation / scale:.{precision}f}"


def print_summary(records: Sequence[Mapping[str, str]]) -> None:
    grouped: Dict[Tuple[str, int], List[Mapping[str, str]]] = {}
    for record in records:
        key = (record["dataset"], int(record["slots"]))
        grouped.setdefault(key, []).append(record)

    print("\nMeasured summary (mean +/- sample stdev)")
    for dataset in sorted({key[0] for key in grouped}):
        print(f"\ndataset: {dataset}")
        print(
            "slots  n  elapsed_s             peak_rss_MiB          "
            "retired_MiB           live_requested_MiB   sidecar_MiB"
        )
        for slots in SLOTS:
            rows = grouped[(dataset, slots)]
            elapsed = [float(row["elapsed_seconds"]) for row in rows]
            peak = [float(row["peak_rss_bytes"]) for row in rows]
            retired = [float(row["gc_retired_bytes"]) for row in rows]
            live = [float(row["live_requested_bytes"]) for row in rows]
            sidecar = [
                float(row["live_sidecar_requested_bytes"]) for row in rows
            ]
            print(
                f"{slots:>5} {len(rows):>2}  "
                f"{pair(elapsed, 1.0, 6):<21} "
                f"{pair(peak, MIB, 3):<21} "
                f"{pair(retired, MIB, 3):<21} "
                f"{pair(live, MIB, 3):<20} "
                f"{pair(sidecar, MIB, 3)}"
            )


def main(argv: Sequence[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        binary = resolve_binary(arguments.binary)
        datasets = resolve_datasets(arguments.datasets)
        output = Path(arguments.output).expanduser().resolve()
        protected_inputs = {binary, *datasets}
        if output in protected_inputs:
            raise BenchmarkError(
                f"CSV output must not overwrite a benchmark input: {output}"
            )
        run_warmups(
            binary,
            datasets,
            arguments.warmups,
            arguments.clients,
            arguments.pipes,
            arguments.gc,
        )
        records = run_measurements(
            binary,
            datasets,
            arguments.runs,
            arguments.warmups,
            arguments.clients,
            arguments.pipes,
            arguments.gc,
        )
        write_csv(output, records)
        print_summary(records)
        print(f"\nCSV: {output.expanduser().resolve()}")
        return 0
    except BenchmarkError as error:
        print(f"fat_slots_bench: error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
