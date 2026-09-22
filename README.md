# Contrees
## Requirement
- An x86-64(AMD64) platform supporting SIMD and AES-NI
- C++ compiler supporting C++20
- Mimalloc v1.8.4(v2.1.4) or above

## Building
To build the main project:
```sh
make
```

## Export Dataset
```bash
make gen
```

This command will retrieve all files in the `ycsbc/workloads` directory, generate the corresponding datasets based on these configuration files, and store them in `ycsbc/export`.

## Usage
Run the main benchmark:
```sh
./main <dataset> <scheduler> <structure> [options]
```
- `<dataset>`: Path to a binary workload file
- `<scheduler>`: `inplace`, `seqcow`, `concow`, or `concow-fat`
- `<structure>`: `betree`, `btree`, `aert`, `art`
- Options:
  - `-c <num_clients>`: Number of client threads (default: 1)
  - `-p <num_pipes>`: Number of pipes (default: 1)
  - `-w <num_workers>`: Number of concurrent workers (default: 1)
  - `-g`, `--gc`: Enable reader-version based reclamation (default: off)
  - `--fat-slots <0|2|4|8>`: Enable the experimental B+Tree/BeTree/ART/AERT leaf
    buffer (default: 2 for `seqcow btree` and `seqcow betree`; explicit 0
    disables it; `concow-fat` supports all four trees and defaults to 2)

When GC is enabled, every query is pinned to the published root/version for
the duration of its callback. Each client id may therefore have at most one
outstanding query. Writers retire only physical nodes replaced by path copy;
the collector frees a retirement batch after both the oldest active reader and
the continuous committed-writer frontier have reached its retirement version.
The program reports retired, reclaimed, and pending node/byte counts.

For BeTree checkpoints, the collector distinguishes a full rebuild from an
upper-part rebuild. It retires the whole old tree after a full rebuild, or only
the replaced upper shell when the rebuilt root reuses lower subtrees.

### Experimental fat-leaf mode (phase 1)

Fat mode lazily attaches a 2, 4, or 8-entry version buffer to each B+Tree or
BeTree leaf. An update appends `{version, key, value}` to the target leaf
without changing the root. When that particular leaf's buffer is full, the
next update merges the base leaf, visible buffered records, and the new value,
then uses that tree's original path-copy code to replace or split the
materialized leaf. The limit is therefore per physical leaf, not one global
"copy every N updates" counter.

Readers receive one consistent root/version snapshot. Point and range queries
select only buffered records whose version is visible to that snapshot. A
sidecar is reclaimed together with its leaf, so this mode can be combined with
`--gc`; appending a record retires nothing, while materialization retires the
replaced leaf and path through the existing GC mechanism.

![Fat-node memory optimization overview](fat-node-overview.png)

The diagram describes the independent B+Tree ordered-wave controller, retained
as a mutex-based batching comparison, with existing [ART](docs/ART_CONCOW_FAT.md),
[AERT](docs/AERT_CONCOW_FAT.md), and [BeTree](docs/BETREE_CONCOW_FAT.md) variants.
Current development focuses on [native cyclic B+Tree](docs/CONCOW_CYCLIC_FAT.md).
Its path-local readiness probe preserves the original staged execution.
The native cyclic write/commit path, including GC, uses no application mutex;
see [monitor-owned reclamation](docs/CONCOW_CYCLIC_GC.md). Original stage waits
remain, as agreed for this project; this does not assert strict lock-free
progress under arbitrary thread suspension ([audit](docs/CONCOW_PROGRESS_AUDIT.md)).
The diagram's measurements remain specific to the independent B+Tree controller.

For a full BeTree checkpoint, visible sidecar records are materialized into
the rebuilt leaves. A partial checkpoint continues to share its lower
subtrees, including their sidecars; checkpoint reclamation stops before those
shared nodes.

This first phase does not add buffers to internal nodes or prune individual
records in place. B+Tree now also supports the original cyclic ConCow pipeline;
see [native pipeline integration](docs/CONCOW_CYCLIC_FAT.md). Two slots is
the independently measured memory-first default for `seqcow btree` and
`seqcow betree`; four is the measured throughput option for sustained
write-heavy loads. With `--fat-slots 0`, the corresponding original update
path remains selected. Run the focused correctness suites with:

```sh
make test-fat-btree
make test-fat-betree
```

Build the dedicated tuning driver and compare 0/2/4/8 in counterbalanced
processes with:

```sh
make bench-fat-btree
python3 bench/fat_slots_bench.py \
  ./bench/btree_fat_bench \
  ycsbc/export/workload_fat_uniform_update.data \
  ycsbc/export/workload_fat_zipfian_update.data \
  ycsbc/export/workload_fat_lookup90.data \
  --runs 5 --warmups 1 --clients 1 --gc \
  --output bench/results/fat_slots.csv
```

For BeTree, build `make bench-fat-betree`, select
`./bench/betree_fat_bench`, and pass `--pipes 3` to the runner for the
repository's four-stage layout. The repository-local ARM64 tuning reports and raw
measurements are `bench/results/fat_slots_arm64_summary.md` for B+Tree and
`bench/results/betree_fat_slots_arm64_summary.md` for BeTree.

The driver reports both process RSS and requested live tree/sidecar bytes.
`gc_retired_bytes` is cumulative allocation traffic, not live memory.
Implemented scope, deferred designs, and per-tree acceptance criteria are
tracked in `FUTURE_WORK.md`.

## Remark
The number of pipes does not include the last one, so it should be one less than the number reported in the paper.

## References
- [YCSB-C](https://github.com/brianfrankcooper/YCSB/wiki): Standardized cloud serving benchmark.
- [Mimalloc](https://github.com/microsoft/mimalloc): Used for efficient concurrent memory allocation.

### ART / AERT fat leaves

SeqCow also accepts `--fat-slots 2/4/8` for ART and AERT. Two records fit in
existing leaf padding: the leaf remains 64 bytes, with no side allocation in
slot-2 mode. Slot-4/8 overflow is allocated on the third update. Existing-key
updates preserve the root until slots fill; inserts use COW. Point queries and
range scans select values at the pinned snapshot version. ConCow is unsupported.
Defaults remain 0 pending full allocator/platform validation; explicitly select
2 for the smallest enabled live-memory footprint.

Use `make test-fat-radix bench-fat-radix`. See
[design, results and validation limits](bench/results/radix_fat_design.md)
for native ARM64 testing without mimalloc and reproducible memory commands.

On macOS, benchmark targets detect mimalloc in `~/.brew`, `/opt/homebrew`, or
`/usr/local`; use `MIMALLOC_PREFIX` to override. Run `make -B test-fat-radix
bench-fat-radix CC=clang++` after switching allocator flags. The
[Mac mimalloc validation report](bench/results/mac_mimalloc_validation.md)
contains the 100k/1m results and current sanitizer environment limitations.

### Experimental concurrent fat-node controller

The ordered-wave controller now supports B+Tree, ART, AERT, and BeTree. Workers
append to different leaves concurrently; updates to the same leaf stay ordered.
Replacement leaves may be prepared concurrently, then the coordinator copies each
affected shared path once before publishing the complete root/version pair. BeTree
also preserves elastic boundaries and performs reader-safe full or partial
checkpoints. All four trees also have native ConCow fat-node support; see [native BeTree/ART/AERT](docs/NATIVE_MULTITREE_FAT.md) for the conservative entry-append protocol and limits.

Build the focused suites with `make test-concow-fat test-concow-art
test-concow-aert test-concow-betree CC=clang++`. Select the independent controller
in the main CLI with `./main <dataset> concow-fat <tree> -g --fat-slots 2`.
Dedicated benchmark binaries provide reproducible comparisons.

See [the protocol and limitations](docs/CONCOW_FAT_DESIGN.md),
[the SeqCow rollback baseline](docs/SEQCOW_BASELINE.md), and
[the initial experiment](bench/results/concow_fat_btree_summary.md), and
[the dispatch tuning report](bench/results/concow_fat_dispatch_summary.md).

Tree-specific implementations and results are in [ART](docs/ART_CONCOW_FAT.md),
[AERT](docs/AERT_CONCOW_FAT.md), and [BeTree](docs/BETREE_CONCOW_FAT.md).
The combined status and comparable measurements are in the
[four-tree summary](bench/results/concow_fat_multitree_summary.md).

See also [parallel materialization results](bench/results/concow_fat_materialization_summary.md).

Phase timing and dispatch-threshold measurements are in the
[B+Tree bottleneck analysis](bench/results/concow_fat_profile_summary.md).

B+Tree 原 cyclic 调度新增显式 `--fat-workers`：入口预留槽，worker 并发发布，
物化拆为结构准备与 worker 填值。协议与测试见 [worker fat-node](docs/CONCOW_WORKER_FAT.md)。
默认仍保留入口追加模式，吞吐扩展性需独立评估。

BeTree、ART、AERT 已支持原固定流水线 `concow <tree> --gc --fat-slots 2/4/8`，
使用无互斥锁的 monitor 回收协议；第一版为入口追加，`--fat-workers` 仍仅支持 B+Tree。
[协议、checkpoint 与验证](docs/NATIVE_MULTITREE_FAT.md)。
