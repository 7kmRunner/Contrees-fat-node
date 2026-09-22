# BeTree 新架构本地实验

本文保留首轮 144 条历史测量。当前 checkpoint 修复和 180 条同容量实验见
[更新报告](concow_betree_checkpoint_fixed_summary.md)：相同槽数下新架构更省内存，
但测得耗时均比 SeqCow 更长，不能以本文 Batch8/Seq2 对比宣称架构本身更快。

平台为 macOS 26.6.2 ARM64、Apple clang 17、mimalloc 3.5，编译参数为
C++20/O3/NDEBUG。每个进程从 100,000 个键开始，执行 100,000 次操作，4 个
客户端提交，GC 开启。每配置预热一次、正式测量三次；48 个配置共 144 条正式
结果。耗时和内存均为三次中位数。

`seq` 是已有 SeqCow BeTree fat-node，`batch` 是新有序批次控制器。`cow` 是
独立控制器逐操作路径复制对照，不代表原论文 ConCow。SeqCow 使用 65,536 个
context，新控制器使用 4,096 项队列，发布和 GC 频率也不同，因此 RSS 变化包含
控制器成本。

## 串行 slots=2 与新架构单 worker

| 负载 | 固定旧快照 | Seq2 ms / MiB | Batch2 ms / MiB | Batch8 ms / MiB | Batch8 加速比 | Batch8 RSS 降幅 |
|---|---:|---:|---:|---:|---:|---:|
| uniform | 0 | 17.66 / 43.72 | 23.82 / 20.44 | 14.52 / 21.16 | 1.22x | 51.6% |
| uniform | 1 | 17.29 / 72.84 | 24.28 / 44.55 | 13.56 / 26.53 | 1.28x | 63.6% |
| hot | 0 | 9.83 / 46.22 | 36.52 / 20.42 | 21.14 / 20.48 | 0.47x | 55.7% |
| hot | 1 | 10.90 / 74.95 | 40.61 / 43.12 | 23.28 / 31.22 | 0.47x | 58.3% |
| insert | 0 | 15.58 / 56.59 | 22.82 / 24.25 | 14.98 / 24.94 | 1.04x | 55.9% |
| insert | 1 | 16.30 / 95.47 | 23.41 / 46.78 | 15.33 / 33.78 | 1.06x | 64.6% |

Batch2 在六种情形中都降低峰值 RSS，但比 Seq2 慢。Batch8 以 1.26--2.08 MiB
最终 sidecar 换取更少物化，在均匀更新和插入中同时降低耗时和峰值 RSS；热点
更新仍约为 Seq2 耗时的 2.1 倍。这里将新架构的实测较优容量与 SeqCow 的既有
内存默认值比较，槽容量不同，结论应理解为可选配置的系统结果，而不是只隔离
合并算法的微基准。

## Batch2 worker 数量，时间 ms

| 负载 | 固定旧快照 | 1 worker | 2 workers | 4 workers |
|---|---:|---:|---:|---:|
| uniform | 0 | 23.82 | 32.65 | 38.08 |
| uniform | 1 | 24.28 | 31.54 | 35.95 |
| hot | 0 | 36.52 | 35.77 | 35.95 |
| hot | 1 | 40.61 | 37.88 | 37.90 |
| insert | 0 | 22.82 | 40.31 | 44.48 |
| insert | 1 | 23.41 | 39.40 | 44.59 |

热点场景的两个 worker略快于单 worker，但仍远慢于 SeqCow；其余场景单 worker
最快。当前证据不能支持普遍的 worker 扩展性结论。

所有正式运行都完成最终全量键值检查和固定旧快照检查，并满足 pending=0、
retired=reclaimed。功能测试覆盖 24 组控制器配置、并发物化、异常不发布、两层
级联分裂和 Boundary checkpoint。mimalloc 与系统分配器 UBSan 通过，另外三棵
树的新架构测试和四棵树的串行 fat-node 回归通过。ASan/TSan 按既定决定搁置，
不记为通过。

原始数据为 `concow_betree_arm64.csv`。复现：

```sh
make test-concow-betree bench-concow-betree CC=clang++
python3 bench/concow_betree_matrix.py
```

这是本机小规模结果，不代替 x86-64 论文规模实验。源码和二进制摘要见
`concow_betree_metadata.json`。
