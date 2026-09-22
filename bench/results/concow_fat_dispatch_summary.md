# B+Tree 并发 fat-node：小批次派发优化

## 本轮变化与范围

在第一阶段已验证的有序批次协议上增加 `parallel_min_groups`，默认值为 16。
少于 16 个独立叶分组的批次由 coordinator 直接执行；达到阈值且有多个 worker
才进入 worker 池。结构物化仍在全局屏障后串行执行，没有改变快照、容量预留或 GC 协议。
参数仅作用于独立实验控制器，不调整正式 ConCow/SeqCow 配置。

新增 `parallel_waves`、`parallel_updates`，区分配置的线程数与实际派发到并行
批次的更新数。这个计数表示派发工作量，不能逐次证明线程同时运行；确定性门控
测试另外证明了默认阈值下确实有两个 worker 同时写不同叶。

这是减少无效派发的优化，不是已获得多 writer 吞吐扩展性的证据。
旧结果保留在 [第一阶段报告](concow_fat_btree_summary.md)，优化前源文件和驱动
归档在 `build/baselines/concow-before-dispatch-tuning.tar.gz` 及
`build/concow_fat_bench_before_tuning`。

## 实验方法

Mac ARM64、macOS 26.6.2、Apple clang 17、mimalloc 3.5.0、-O3 -DNDEBUG。
仍为 10,000 初始记录、30,000 更新、4 client、GC on、队列 4096、批次 256。
同一可执行文件对比阈值 2 与 16，每配置 1 次 warmup、3 次测量；阈值与 worker
顺序交替，slots 0/2/4/8 轮换。Uniform/hot/有限 Zipfian，无长读者/初始版本长 pin，
1/2/4 worker，共 432 条测量，全部参考模型正确、GC pending bytes=0。
没有与编译或其它测试并行运行测量。

0 槽是新控制器的强制物化 COW 对照；不是原 ConCow 或原 SeqCow，不能混称。

## 两个 worker：派发阈值对时间的影响

Uniform、无长读者，时间秒（3 次均值）。最后一列是阈值 16 的并行批次更新数
除以全部 30,000 次更新，包含结构更新在分母中。

| 槽数 | 阈值 2 | 阈值 16 | 时间下降 | 进入并行批次的更新 |
|---|---:|---:|---:|---:|
| 0 | 0.0296 | 0.0303 | -2.4% | 0.00% |
| 2 | 0.0688 | 0.0258 | 62.5% | 3.15% |
| 4 | 0.0582 | 0.0197 | 66.2% | 17.90% |
| 8 | 0.0371 | 0.0161 | 56.6% | 50.63% |

0 槽不经过追加批次，阈值对其没有算法影响；两列差异反映运行波动。
16 是本轮有效的启发式配置，不宣称它是所有规模和机器上的最优阈值。

## 阈值 16：长读者下的峰值内存

两个 worker，RSS MiB（3 次均值）：

| 分布 | 0 槽 | 2 槽 | 4 槽 | 8 槽 |
|---|---:|---:|---:|---:|
| uniform | 43.672 | 17.292 | 11.891 | 8.427 |
| hot | 43.672 | 17.250 | 11.953 | 8.766 |
| zipfian | 43.844 | 17.464 | 12.078 | 8.552 |

无长读者时 RSS 仍约 8.2 MiB，各槽数差异很小。fat sidecar 增加最终树大小，
因此不声称在所有读者寿命下都能节约内存。完整 live/sidecar/retired 字节见原始 CSV。

## 阈值 16：worker 数量仍未带来吞吐扩展

Uniform、无长读者，时间秒：

| 槽数 | 1 worker | 2 workers | 4 workers |
|---|---:|---:|---:|
| 0 | 0.0270 | 0.0303 | 0.0274 |
| 2 | 0.0245 | 0.0258 | 0.0268 |
| 4 | 0.0163 | 0.0197 | 0.0218 |
| 8 | 0.0101 | 0.0161 | 0.0187 |

当前单 worker 仍最快。阈值改善了多 worker 相比初版的开销，但 2 槽绝大多数
更新仍在 coordinator 执行。不能把“避免无益并行”解释为“并行加速”。

## 验证与复现

优化构建、mimalloc UBSan、系统分配器 UBSan 均通过。默认阈值 16 的确定性
双 worker 重叠测试、GC 开/关的未来版本隐藏测试通过；较低阈值下继续覆盖
多 client 参考模型、同叶冲突、部分并行批次失败等路径。
ASan/TSan 按用户要求暂时搁置，不标记为通过。

```sh
make test-concow-fat bench-concow-fat CC=clang++
python3 bench/concow_fat_matrix.py --parallel-min-groups 2 16 \
  --output bench/results/concow_fat_btree_dispatch_arm64.csv
```

[原始 CSV](concow_fat_btree_dispatch_arm64.csv)；
[编译与源码校验信息](concow_fat_dispatch_metadata.json)；
[协议设计](../../docs/CONCOW_FAT_DESIGN.md)。

下一步的主要问题已经从频繁唤醒转向批次被结构操作过早截断。应先研究在多个
独立叶上并行准备物化结果、由 coordinator 合并路径的协议，或比较其它方案；
需要重新证明历史快照和退休节点安全，不能直接把当前实现扩展成并发结构写。
这是该轮调优时的后续建议；目前其它三棵树的第一阶段适配和并发验收已经完成，
更大规模 x86-64 实验仍待补。
