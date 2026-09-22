# Parallel ConCow GC memory benchmark

## Method

- Initial records: 100,000.
- 1% workload: 1,000 insertions.
- 100% workload: 100,000 insertions.
- Scheduler: `concow`; 32 client threads.
- ART and B+Tree: `-p 1 -w 8` (two pipeline stages, eight workers).
- AERT and BeTree: `-p 3 -w 16` (four pipeline stages, sixteen workers).
- Binary: x86-64 original synchronization path under Rosetta, using the system
  allocator. The ARM64 portability build was not used because its ConCow smoke
  test failed in both GC-off and GC-on. The original synchronization path is
  x86-oriented; the ARM failure was not repaired or attributed to GC.
- Each entry is the arithmetic mean of three independent processes. GC off/on
  order alternated between repetitions.
- Peak RSS is whole-process resident memory measured with `wait4(2)`.

## 1% (1,000 insertions)

| Structure | GC off mean RSS (MiB) | GC on mean RSS (MiB) | Saved (MiB) | RSS reduction | Elapsed change |
|---|---:|---:|---:|---:|---:|
| ART | 40.27 | 39.66 | 0.61 | 1.52% | +44.48% |
| AERT | 38.46 | 39.32 | -0.86 | -2.22% | -13.45% |
| B+Tree | 29.10 | 27.76 | 1.34 | 4.60% | +8.28% |
| BeTree | 29.30 | 28.65 | 0.65 | 2.21% | -10.30% |

The 1% workload is too short for small RSS or elapsed-time differences to be
interpreted as a stable ranking. Fixed thread stacks, scheduler state, process
startup, and allocator behavior dominate only 1,000 insertions.

## 100% (100,000 insertions)

| Structure | GC off mean RSS (range) | GC on mean RSS (range) | Saved mean (MiB) | RSS reduction | Elapsed change |
|---|---:|---:|---:|---:|---:|
| ART | 613.89 (587.79--638.68) | 260.32 (255.40--264.27) | 353.58 | 57.60% | +3.69% |
| AERT | 243.71 (217.18--293.09) | 95.16 (92.59--96.53) | 148.55 | 60.95% | -7.55% |
| B+Tree | 1024.76 (478.25--1504.19) | 44.98 (40.19--53.57) | 979.78 | 95.61% | +4.44% |
| BeTree | 262.20 (194.19--348.07) | 100.45 (95.47--104.99) | 161.75 | 61.69% | -1.22% |

All 24 GC-on runs across the two workload sizes finished with reclaimed
nodes/bytes equal to retired nodes/bytes and zero pending nodes/bytes. For the
100% workload, each run reclaimed 285,292 nodes / 430,559,488 bytes for ART,
485,292 nodes / 84,959,488 bytes for AERT, and 600,000 nodes / 153,600,000
bytes for each of B+Tree and BeTree.

The wide GC-off ranges, especially B+Tree, are consistent with
schedule-dependent speculation/retry and system-allocation high-water effects;
this test does not instrument their individual contributions. GC-on bounds the
accumulation and has a much narrower range. B+Tree's 95.61% is therefore an
exploratory three-run mean, not a precise constant (even the conservative
off-minimum/on-maximum comparison still reduces RSS by about 88.8%). Reclaimed
logical bytes and RSS reduction need not match because RSS includes allocator
fragmentation/caches, input arrays, thread stacks, and scheduler state.

## Scope

This is a parallel GC stress comparison, not a formal reproduction of the
paper. ConCow executes updates in parallel across clients, pipeline stages, and
workers, but commits one continuous version prefix; reclamation is performed by
the monitor. The insert-only workload has no active readers, so it measures the
best-case reclamation frontier and does not exercise a long-lived reader
holding an old version. The x86/Rosetta/system-allocator results must not be
compared directly with the earlier ARM64/mimalloc sequential RSS values.
