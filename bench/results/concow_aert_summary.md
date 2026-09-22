# AERT 新架构本地实验

平台为 macOS 26.6.2 ARM64、Apple clang 17、mimalloc 3.5，编译参数为
C++20/O3/NDEBUG。每个进程从 100,000 个键开始，执行 100,000 次操作，
由 4 个客户端提交，GC 开启。每个配置预热一次、正式测量三次；48 个配置共
144 条正式结果。下表为三次中位数。

`seq` 是已有 SeqCow AERT fat-node，`batch` 是共享的有序批次控制器。
`cow` 是独立控制器逐操作路径复制的协议对照，不代表原论文 ConCow。
SeqCow 使用 65,536 个 context，新控制器使用 4,096 项队列，而且两者发布和
GC 频率不同，因此 RSS 变化包含控制器开销，不能全部归因于叶槽。

## 单 worker、slots=2

| 负载 | 固定旧快照 | Seq 时间 ms | 新架构时间 ms | Seq/新架构 | Seq RSS MiB | 新架构 RSS MiB | RSS 降幅 |
|---|---:|---:|---:|---:|---:|---:|---:|
| uniform | 0 | 5.62 | 9.00 | 0.62x | 33.38 | 23.41 | 29.9% |
| uniform | 1 | 5.81 | 8.22 | 0.71x | 38.44 | 27.62 | 28.1% |
| hot | 0 | 8.25 | 16.16 | 0.51x | 45.16 | 22.86 | 49.4% |
| hot | 1 | 8.60 | 16.59 | 0.52x | 64.67 | 30.89 | 52.2% |
| insert | 0 | 20.65 | 18.56 | 1.11x | 77.02 | 32.81 | 57.4% |
| insert | 1 | 20.90 | 20.95 | 1.00x | 146.19 | 72.12 | 50.7% |

新架构在六种情形中均降低峰值 RSS。普通更新仍由 SeqCow 更快；无固定快照的
插入快 11%，固定快照的插入耗时基本相同。插入不使用历史槽，收益来自共享路径
合并和较小的控制器状态。

## batch worker 数量，slots=2，时间 ms

| 负载 | 固定旧快照 | 1 worker | 2 workers | 4 workers |
|---|---:|---:|---:|---:|
| uniform | 0 | 9.00 | 17.00 | 21.45 |
| uniform | 1 | 8.22 | 16.22 | 20.51 |
| hot | 0 | 16.16 | 54.02 | 67.97 |
| hot | 1 | 16.59 | 54.72 | 69.48 |
| insert | 0 | 18.56 | 26.37 | 30.38 |
| insert | 1 | 20.95 | 28.52 | 32.32 |

单 worker 仍最快。测试证明了多个 worker 的执行正确性，没有证明吞吐随 worker
数量扩展。uniform 场景的 slots=4/8 单 worker 分别为 8.09/7.29 ms，代价是
最终 overflow 从 0 增到 0.29/0.80 MiB；热点场景分别为 14.39/12.50 ms。
容量更大减少物化，但增加常驻历史记录，需要按负载选择。

所有正式运行都完成最终全量键值检查和固定旧快照检查，并满足 pending=0、
retired=reclaimed。24 组功能矩阵与 tagged pointer 展开专项测试通过；mimalloc
和系统分配器 UBSan 通过；ART、B+Tree 并行及 radix 串行回归通过。
ASan/TSan 按既定决定搁置，不记为通过。

原始数据为 `concow_aert_arm64.csv`，复现命令：

```sh
make test-concow-aert bench-concow-aert CC=clang++
python3 bench/concow_aert_matrix.py
```

这是本机小规模结果，不代替 x86-64 论文规模实验。源码和二进制摘要见
`concow_aert_metadata.json`。
