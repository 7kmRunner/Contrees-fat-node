# GC memory benchmark

`memory_bench.py` runs the same workload with GC disabled and enabled, then
reports wall time and peak resident set size (RSS). In structured mode, GC is
off by default and the runner adds `-g` for the GC-on command.

The runner obtains per-child resource usage with `wait4(2)`. This is the same
kernel accounting used by `/usr/bin/time`, while avoiding the incompatible
Darwin (`time -l`, bytes) and GNU (`time -v`, KiB) output formats. RSS is
normalized to KiB in the CSV.

## Run

Build the executable and generate or supply a binary workload, then run:

```sh
make bench-memory \
  DATASET=ycsbc/export/workload_update.bin \
  BENCH_SCHEDULER=seqcow \
  BENCH_STRUCTURE=art \
  BENCH_CLIENTS=4 \
  BENCH_RUNS=5
```

The default output is a timestamped CSV under `bench/results/`, accompanied by
one program log per run. Set `BENCH_OUTPUT=path/to/result.csv` for a fixed path.
GC-off and GC-on execution order alternates between repetitions to reduce
systematic first-run bias.

The Python runner can also be invoked directly:

```sh
python3 bench/memory_bench.py \
  --binary ./main \
  --dataset ycsbc/export/workload_update.bin \
  --scheduler seqcow \
  --structure art \
  --clients 4 \
  --runs 5 \
  --warmups 1
```

For custom executables or compile-time GC variants, pass two complete commands:

```sh
python3 bench/memory_bench.py --runs 5 \
  --off-command './main-nogc data.bin seqcow art -c 4' \
  --on-command './main-gc data.bin seqcow art -c 4'
```

Commands are parsed without a shell and should invoke the measured executable
directly rather than through another script.

## Paper-aligned 1% update workload

The paper's memory experiment starts with 100 million records and applies 1
million insertions (1%), averaging ten independent runs. Generate the matching
repository workload with:

```sh
cd ycsbc
make ycsbc
./ycsbc -db export -P workload_memory_1pct
```

`workload_memory_1pct_scaled` preserves the same 1% ratio at 10 million records
and 100 thousand insertions for machines that cannot run the full server-scale
experiment without memory pressure. The exported file is named after the
workload under `ycsbc/export/`.

The paper uses 32 clients. For AERT and BeTree it reports four pipeline stages
and 16 workers; this repository's `-p` argument excludes the final stage, so
the corresponding command uses `--pipes 3`. ART and B+Tree use the in-place
baseline for the paper's cross-structure memory ratio. Run those baselines in
separate processes so peak RSS is never shared between variants.

The runner reports whole-process RSS. Unlike a tree-only allocation counter,
it includes the workload arrays described below. Results from a scaled
workload, a different allocator/architecture, or `seqcow` are useful GC
comparisons but are not a formal reproduction of the paper's absolute ratios.

On a non-x86 development machine, the portable BeTree-only driver exercises
the same path-copy handlers, schedulers, reader protocol, and reclaimer without
compiling the ART/AERT SIMD headers:

```sh
make bench-gc-native
python3 bench/memory_bench.py --runs 5 \
  --off-command './bench/betree_gc_bench --updates 200000' \
  --on-command './bench/betree_gc_bench --updates 200000 --gc'
```

Run its pinned-reader safety scenario separately with:

```sh
./bench/betree_gc_bench --gc --reader-safety-test --updates 20000
```

Exercise both BeTree checkpoint reclamation cases (an upper-shell rebuild that
shares lower subtrees, and a full rebuild) with:

```sh
./bench/betree_gc_bench --checkpoint-reclaim-test --records 100000
```

## Interpreting RSS

Peak RSS is a whole-process measurement, not tree-only live bytes. The current
benchmark loads all input before execution, so every result includes at least:

- `elems`: approximately `8 * num_records` bytes;
- `kvs`: approximately `16 * num_records` bytes;
- `txs`: approximately `24 * num_transactions` bytes on x86-64;
- the current tree, unreclaimed/retired nodes, scheduler state, thread stacks,
  executable pages, and mimalloc caches.

The runner prints the first three arrays' estimated lower bound and writes it
to the CSV. This fixed component is identical between paired modes, but it can
hide a tree-memory improvement when the workload file is very large. Prefer a
long update-heavy workload, repeat it several times, and compare both peak RSS
and elapsed-time overhead. Mimalloc may retain freed pages, so a lower count of
live nodes does not always produce an immediate proportional RSS drop.

For correctness, compare GC on/off with the same dataset, scheduler, structure,
client count, and process environment. A failed run remains in the CSV with its
exit code and log path.

## Fat-leaf tuning

`fat_slots_bench.py` compares capacities 0/2/4/8 in fresh processes and rotates
their execution order. The compatible B+Tree and BeTree drivers report process
RSS, exact requested bytes reachable from the final root, sidecar bytes, and GC
allocation/reclamation counters.

```sh
make test-fat-btree test-fat-betree
make bench-fat-btree bench-fat-betree

python3 bench/fat_slots_bench.py \
  ./bench/betree_fat_bench \
  ycsbc/export/workload_fat_uniform_update.data \
  ycsbc/export/workload_fat_zipfian_update.data \
  --runs 5 --warmups 1 --clients 1 --pipes 3 --gc \
  --output bench/results/betree_fat_slots.csv
```

`--pipes` is optional and should be omitted for the B+Tree driver. The included
ARM64 reports document the compiler, workload matrix, raw CSV, caveats, and the
memory-first default decision for each implemented tree.

## ART / AERT

`make test-fat-radix bench-fat-radix` builds the shared radix correctness suite
and ART/AERT drivers compatible with `fat_slots_bench.py`. See
[the design and measurement report](results/radix_fat_design.md) for allocation
layout, system-allocator fallback, reproducible inputs and sanitizer limitations.

### macOS mimalloc

Makefile now detects mimalloc under `~/.brew`, `/opt/homebrew`, or `/usr/local`.
Override with `MIMALLOC_PREFIX=/path/to/prefix`. Use `make -B` when switching
allocator/compiler flags so existing benchmark binaries are rebuilt.
Native test and benchmark commands and 100k/1m measurements are in
[the Mac mimalloc report](results/mac_mimalloc_validation.md).

## Experimental ConCow B+Tree

```sh
make test-concow-fat bench-concow-fat CC=clang++
python3 bench/concow_fat_matrix.py --output bench/results/concow_fat_btree.csv
```

This driver measures the separate ordered-wave controller with 1/2/4 workers,
0/2/4/8 slots, four concurrent submitting clients, and optional long-lived reader
pins. Every run verifies the final tree against returned-ticket order and checks
that GC drains. Slot 0 is forced materialization in the same new controller, not
legacy ConCow or SeqCow. See [the report](results/concow_fat_btree_summary.md).
ASan/TSan remain unverified on this Mac; the focused tests run under UBSan.

The experimental controller now keeps small waves on the coordinator (default
`parallel_min_groups=16`). Compare `--parallel-min-groups 2 16` with the matrix
runner; the CSV reports actual parallel-wave update counts. See
[dispatch tuning results](results/concow_fat_dispatch_summary.md).

Compare append-only waves with parallel leaf materialization using the same binary:

```sh
python3 bench/concow_fat_matrix.py --materialization 0 1 --output bench/results/concow_fat_btree_materialization_arm64.csv
```

The new mode keeps one materialization per leaf per wave, then merges shared
paths on the coordinator. `parallel_materializations` counts materializations
dispatched to worker waves; deterministic gate tests separately verify overlap.
`parallel_updates` now includes both appends and materializations in those waves.
Default mode remains 0. See [the comparison](results/concow_fat_materialization_summary.md).

Build `make profile-concow-fat CC=clang++` and run `python3 bench/concow_fat_profile.py`
to compare phase-instrumented and ordinary builds. `--tune --output <csv>` measures
dispatch thresholds without instrumentation. Worker times overlap the parallel
wave's wall time and must not be added to it. See the
[bottleneck analysis](results/concow_fat_profile_summary.md).

## Native cyclic ConCow integration

Since 2026-09-17, cyclic retirement and reclamation are owned by the monitor;
external collectors request a pass instead of locking the retirement queue.
Run `make test-concow-cyclic-gc CC=clang++` for concurrent collectors, a paused
collector, retained snapshots, and idle reclamation. See the
[GC ownership protocol](../docs/CONCOW_CYCLIC_GC.md). The September 16 path-probe
measurements below predate this GC change and remain historical results.

The current development path extends `concow_cyclic` itself. Build with
`make test-concow-cyclic-fat test-concow-progress bench-concow-cyclic-fat CC=clang++`,
then run `python3 bench/concow_path_probe_matrix.py`. This compares slots
0/2/4/8, global/local admission probes, 1/2/4 workers, uniform/hot updates,
and pinned/unpinned snapshots using the same native pipeline binary.
The older `concow_cyclic_fat_matrix.py` also supports the historical paced workload.
The main CLI also accepts `concow btree --fat-slots 2/4/8`.
See [the protocol](../docs/CONCOW_CYCLIC_FAT.md) and
[path-probe measurements](results/concow_path_probe_summary.md). The old global
gate remains available with `--global-probe`; the default checks construction
readiness along the target path. The progress test deliberately reproduces a
submission-owner stall, rather than asserting lock-free success. Read the
[progress audit](../docs/CONCOW_PROGRESS_AUDIT.md) before using any no-lock claim.
Independent-controller speedups do not apply to this implementation.
