# Local GC memory results

Date: 2026-08-27 (Asia/Shanghai)

## Workload and measurement

- 10,000,000 initial records and 100,000 insertions (1%).
- Same semantic workload as `workload_memory_1pct_scaled.spec`; the temporary
  filename present in the raw CSVs differs only in transaction-struct padding.
- Native ARM64, mimalloc, SeqCow, one client, three matched repetitions.
- GC-off/GC-on order alternated between repetitions.
- Peak RSS came from `wait4(2)` and includes about 231.17 MiB of always-resident
  input arrays in addition to the tree, scheduler, stacks, and allocator cache.
- ART/AERT used a test-only scalar compatibility header because the repository
  sources require x86 intrinsics. No compatibility code was added to the repo.

## Native paired results

| Structure | GC off peak RSS (MiB) | GC on peak RSS (MiB) | Peak RSS reduction | Retired/reclaimed bytes |
|---|---:|---:|---:|---:|
| ART | 1927.17 | 1433.18 | 25.63% | 636,872,960 |
| AERT | 1422.03 | 1338.46 | 5.88% | 117,703,168 |
| B+Tree | 789.81 | 672.35 | 14.87% | 204,800,000 |
| BeTree | 789.80 | 671.82 | 14.94% | 204,800,000 |

Every GC-on repetition finished with reclaimed nodes equal to retired nodes and
zero pending nodes.

## Cross-structure ratios

Whole-process RSS ratios using the paper's comparison pairs:

| Pair | Path copy, GC off | Path copy, GC on | Paper result |
|---|---:|---:|---:|
| AERT / in-place ART | 1.221x | 1.150x | about 1.15x |
| BeTree / in-place B+Tree | 1.302x | 1.107x | about 2.2x |

These are not formal paper reproductions. The paper uses 100 million records,
1 million operations, 32 clients, ten-run averages, Xeon hardware, and its own
snapshot-retention policy. This project's GC-on mode intentionally drops every
version no active reader needs, while the paper's memory experiment retains a
window of fine-grained snapshots.

## x86 ConCow supplement

A single Rosetta x86-64 run used 32 clients, `-p 3`, and 16 workers with the
same scaled workload. It used the system allocator rather than mimalloc, so it
is a correctness/architecture supplement rather than the primary result:

| Structure | GC off (MiB) | GC on (MiB) | Reduction |
|---|---:|---:|---:|
| AERT | 1823.45 | 1611.08 | 11.65% |
| BeTree | 990.81 | 871.72 | 12.02% |

Both completed normally with zero pending nodes. Rosetta severely distorted
execution time, especially for concurrent frees, so its timing is not used for
performance conclusions.
