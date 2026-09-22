# BeTree fat-slot tuning (ARM64)

## Scope and method

- Native ARM64, macOS 15.7.4, Apple Clang 17, mimalloc 3.5.
- `-O3 -DNDEBUG`, BeTree + SeqCow, GC enabled, one client.
- `num_pipes=3`, which gives the repository's four-stage BeTree layout. In
  SeqCow this changes the BeTree boundary layout; it does not add parallel
  update workers.
- Each sample used a fresh process. One warm-up preceded five measured runs.
- Slots 0/2/4/8 used a counterbalanced Williams order.
- RSS includes the loaded workload and allocator-retained pages. `live
  requested` is the exact requested size of the reachable BeTree nodes plus
  leaf sidecars. `retired` is cumulative allocation traffic, not live memory.
- All 120 measured rows ended with zero pending GC nodes/bytes and exactly
  equal retired/reclaimed totals.

The focused suite separately covers capacities 2/4/8, historical point and
range reads, repeated-key deduplication, insertion and leaf splitting, pinned
readers, randomized reference-model checks, concurrent readers, and both full
and partial BeTree checkpoints. The optimized suite passed normally, under
ASan/UBSan, and under TSan.

## Peak RSS (MiB, mean of five runs)

| Workload | 0 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| 10M records + 100K sparse inserts (1%) | 672.466 | **620.563** | 624.422 | 627.359 |
| 1M records + 1M dense inserts (100%) | 168.266 | 129.694 | **125.053** | 140.309 |
| 1M uniform existing-key updates | 154.453 | 119.191 | **118.069** | 127.694 |
| 1M Zipfian existing-key updates | 154.978 | 118.603 | **116.359** | 122.688 |
| 5M uniform existing-key updates | 247.294 | 210.134 | **208.978** | 219.166 |
| 90% lookup + 10% insert | **95.522** | 98.422 | 100.547 | 107.922 |

Relative to slot 0, slot 2 reduced peak RSS by 7.72% for sparse inserts,
22.92% for dense inserts, 22.83% for uniform updates, 23.47% for Zipfian
updates, and 15.03% for the long update run. It increased peak RSS by 3.04%
in the lookup-heavy workload, where there was too little path-copy allocation
to amortize the sidecars.

## Processing time (seconds, mean of five runs)

| Workload | 0 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Sparse insert | 0.079801 | **0.040809** | 0.041042 | 0.041756 |
| Dense insert | 0.519384 | 0.350636 | 0.267121 | **0.207878** |
| Uniform update | 0.508849 | 0.352476 | 0.267039 | **0.208732** |
| Zipfian update | 0.433680 | 0.254087 | 0.183049 | **0.147027** |
| Long uniform update | 2.558545 | 1.872623 | 1.489121 | **1.278638** |
| 90% lookup + 10% insert | 0.131719 | **0.130317** | 0.132879 | 0.139964 |

Slot 4 remains a throughput-oriented setting. Against slot 2 it was about
20%--28% faster in the sustained-write workloads, while slot 8 was faster
again at a substantially larger reachable-memory cost.

## Live requested memory and cumulative retired bytes

Values are `live requested / retired`, in MiB.

| Workload | 0 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Sparse insert | 348.772 / 195.312 | 353.589 / 0.197 | 357.727 / 0 | 365.994 / 0 |
| Dense insert | 50.156 / 1708.984 | 51.868 / 498.630 | 54.835 / 263.034 | 71.939 / 88.707 |
| Uniform update | 34.877 / 1708.984 | 39.315 / 514.320 | 44.810 / 271.243 | 55.734 / 96.819 |
| Zipfian update | 34.877 / 1708.984 | 38.893 / 524.181 | 43.737 / 292.171 | 53.286 / 148.843 |
| Long uniform update | 34.877 / 8544.922 | 39.326 / 2863.887 | 44.803 / 1717.585 | 56.076 / 949.881 |
| 90% lookup + 10% insert | 34.877 / 170.673 | 38.391 / 9.140 | 41.845 / 0.152 | 48.291 / 0 |

Fat leaves do not make the final reachable tree smaller than slot 0: their
sidecars add exact live bytes. Their memory benefit is reducing repeated path
allocations and therefore allocator-retained/peak RSS. Among the enabled
capacities, slot 2 had the smallest exact live allocation in every workload.

## Decision

Use **2 slots** as the memory-first default for `seqcow + betree`:

- It is the smallest enabled sidecar and the lowest-live-memory enabled mode
  in every workload.
- It delivered most of the peak-RSS benefit in write-heavy workloads and had
  the best enabled peak RSS for sparse and lookup-heavy workloads.
- Larger capacities reduce allocation traffic and improve write throughput,
  but the 4/8-slot reachable-memory increase is contrary to the project's
  memory-first default policy.
- Explicit slot 0 remains useful for read-dominant workloads and selects the
  repository's original path-copy update path.

The lookup-heavy slot-0 checksum was nondeterministic across the five runs,
whereas capacities 2/4/8 agreed exactly. This is consistent with the existing
SeqCow task-registration/copy behavior already documented for the repository;
that slot-0 code was intentionally not repaired as part of this experiment.
Consequently, slot 0 is a memory/allocation reference here, not a proven
correctness-equivalent concurrent baseline.

This is a local tuning experiment, not a reproduction of the paper's x86,
32-client experiment. SeqCow does not execute BeTree checkpoint scheduling;
checkpoint compatibility is covered by the focused tests instead.

Raw measurements: `betree_fat_slots_arm64.csv`.
