# B+Tree fat-slot tuning (ARM64)

## Method

- Native ARM64, macOS 15.7.4, Apple Clang 17, mimalloc 3.5.
- `-O3 -DNDEBUG`, B+Tree + SeqCow, GC enabled, one client.
- Each sample used a fresh process. One warm-up preceded five measured runs.
- Slots 0/2/4/8 used a counterbalanced Williams order.
- RSS includes the loaded workload and allocator-retained pages. `live requested`
  is the exact requested size of the reachable tree plus leaf sidecars.
  `retired` is cumulative allocation traffic and is not live memory.
- Every measured row finished with zero pending GC nodes/bytes and equal
  retired/reclaimed totals.

## Peak RSS (MiB, mean of five runs)

| Workload | 0 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| 10M records + 100K sparse inserts (1%) | 672.666 | **620.578** | 624.438 | 627.375 |
| 1M records + 1M dense inserts (100%) | 168.244 | 129.722 | **125.081** | 140.325 |
| 1M uniform existing-key updates | 154.481 | 119.191 | **118.100** | 127.697 |
| 1M Zipfian existing-key updates | 155.169 | 118.600 | **116.375** | 122.716 |
| 5M uniform existing-key updates | 247.384 | 210.169 | **208.997** | 219.156 |
| 90% lookup + 10% insert | **95.588** | 98.438 | 100.562 | 107.938 |

## Processing time (seconds, mean of five runs)

| Workload | 0 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Sparse insert | 0.082686 | **0.042132** | 0.042870 | 0.042446 |
| Dense insert | 0.527143 | 0.362499 | 0.271469 | **0.216394** |
| Uniform update | 0.516562 | 0.363464 | 0.276053 | **0.222158** |
| Zipfian update | 0.441569 | 0.271313 | 0.198850 | **0.160682** |
| Long uniform update | 2.557723 | 1.979436 | 1.554218 | **1.344868** |
| 90% lookup + 10% insert | 0.167479 | **0.158003** | 0.160078 | 0.165012 |

Several short timing groups had a coefficient of variation above 5%, so the
approximately 1% lookup-time differences between 2/4/8 are not significant.
The 21%--27% write-heavy difference between 2 and 4 is much larger than this
noise.

## Live requested memory and cumulative retired bytes

Values below are `live requested / retired`, in MiB.

| Workload | 0 | 2 | 4 | 8 |
|---|---:|---:|---:|---:|
| Sparse insert | 348.772 / 195.312 | 353.589 / 0.197 | 357.727 / 0 | 365.994 / 0 |
| Dense insert | 50.156 / 1708.984 | 51.868 / 498.630 | 54.835 / 263.034 | 71.939 / 88.707 |
| Uniform update | 34.877 / 1708.984 | 39.315 / 514.320 | 44.810 / 271.243 | 55.734 / 96.819 |
| Zipfian update | 34.877 / 1708.984 | 38.893 / 524.181 | 43.737 / 292.171 | 53.286 / 148.843 |
| Long uniform update | 34.877 / 8544.922 | 39.326 / 2863.887 | 44.803 / 1717.585 | 56.076 / 949.881 |
| 90% lookup + 10% insert | 34.877 / 170.637 | 38.391 / 9.140 | 41.845 / 0.152 | 48.291 / 0 |

## Decision

Use **2 slots** as the memory-first default for `seqcow + btree`:

- Among the enabled capacities, it had the smallest exact live tree allocation
  in every workload. It also had the lowest peak RSS for sparse and read-heavy
  workloads.
- Slot 4 reduced peak RSS by only 0.6%--3.6% relative to 2 in sustained-write
  workloads, while increasing exact live memory by up to 5.5 MiB (13.9%).
- Slot 2 still cut peak RSS by about 15%--23% relative to the original slot-0
  path in the sustained-write runs and sharply reduced allocation traffic.
- Slot 4 remains a useful explicit throughput setting: in the five-million
  update run it was about 21% faster and generated about 40% fewer retired
  bytes than 2. Slot 8 was faster again, but its live and peak memory increases
  make it unsuitable as the memory default.

Use explicit 0 to select the untouched path-copy update path.

The mixed lookup/insert checksum was stable across 2/4/8 but varied for slot 0.
This agrees with the known original SeqCow submission race described below;
slot 0 is retained as the repository-reference path, not treated as a fully
correctness-equivalent concurrent baseline.

The eight-client smoke run completed for every slot count with no pending GC
nodes or bytes. It is not used to tune the capacity because SeqCow still has a
single update worker, and the original slot-0 multi-producer submission path is
not a correctness-equivalent performance baseline for the synchronized fat
path.

Raw measurements:

- `fat_slots_main_arm64.csv`
- `fat_slots_lookup90_arm64.csv`
- `fat_slots_uniform_long_arm64.csv`
- `fat_slots_parallel8_arm64.csv`
