# GC memory curve: 1% versus 100%

## Workload and measurement

- Initial records: 1,000,000.
- 1% workload: 10,000 insertions.
- 100% workload: 1,000,000 insertions.
- Scheduler: `seqcow`; clients: 1; GC off/on run in separate processes.
- Each table entry is the arithmetic mean of three runs. Off/on order alternated.
- Peak RSS is whole-process resident memory measured by `wait4(2)` and normalized
  to MiB. It includes the input arrays, current tree, obsolete nodes, allocator
  caches, scheduler state, stacks, and executable pages.

## 1% (10,000 insertions)

| Structure | GC off RSS (MiB) | GC on RSS (MiB) | Saved (MiB) | RSS reduction | Elapsed change |
|---|---:|---:|---:|---:|---:|
| ART | 187.22 | 175.77 | 11.45 | 6.11% | -2.43% |
| AERT | 135.42 | 133.48 | 1.94 | 1.43% | -1.45% |
| B+Tree | 87.39 | 86.65 | 0.74 | 0.85% | -0.15% |
| BeTree | 87.38 | 80.66 | 6.72 | 7.69% | +1.18% |

The 1% processes last only about 0.06--0.14 seconds. Their small RSS deltas,
especially the allocator-dependent B+Tree/BeTree difference, should be treated
as noisy rather than as a stable cross-structure ranking.

## 100% (1,000,000 insertions)

| Structure | GC off RSS (MiB) | GC on RSS (MiB) | Saved (MiB) | RSS reduction | Elapsed change |
|---|---:|---:|---:|---:|---:|
| ART | 5903.17 | 447.53 | 5455.64 | 92.42% | -29.50% |
| AERT | 1494.28 | 288.43 | 1205.85 | 80.70% | +10.89% |
| B+Tree | 1827.66 | 168.36 | 1659.29 | 90.79% | +12.16% |
| BeTree | 1827.64 | 168.35 | 1659.29 | 90.79% | +12.58% |

Every GC-on run finished with reclaimed nodes/bytes equal to retired
nodes/bytes and zero pending nodes/bytes. At 100%, the GC reclaimed about
4.84 GB for ART, 0.99 GB for AERT, and 1.79 GB for each of B+Tree and BeTree
(decimal byte counts reported by the program).

## Scope

This is a scaled long-run GC comparison, not a formal reproduction of the
paper's server setup. It uses an ARM64 test binary with a scalar compatibility
path for ART/AERT, one client, and `seqcow`; the paper's memory experiment uses
100 million initial records, 1 million insertions (1%), multiple clients, and
different structure-specific baselines. The memory trend is useful here, but
CPU timings should not be presented as native-x86 paper performance.

