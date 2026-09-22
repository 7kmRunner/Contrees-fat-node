# B+Tree 并行物化实验结果

2026-09-09 汇总。Mac ARM64、Apple clang 17、mimalloc；独立实验控制器。
旧模式为有序追加批次与独占结构 COW，新模式为跨叶并行物化及共享路径批量合并。
两种模式在同一二进制中比较，派发阈值 16，4 个提交客户端。
每组 1 次预热、3 次独立进程测量，以下为中位数。

10,000 条记录 / 30,000 次更新：uniform、hot、zipfian，432 条测量。
100,000 条记录 / 300,000 次更新：uniform，144 条测量。
共 576 条记录均通过最终参考模型检查，pending_bytes=0，退休字节全部回收。
优化构建、系统分配器及 mimalloc 的 UBSan 测试通过；ASan/TSan 仍暂缓。

## 100k 记录，2 workers，长读者持续保留初始快照

| 槽数 | 旧耗时 ms | 新耗时 ms | 旧峰值 MiB | 新峰值 MiB |
|---|---:|---:|---:|---:|
| 0 | 423.81 | 144.58 | 488.19 | 240.48 |
| 2 | 308.97 | 91.72 | 173.38 | 102.95 |
| 4 | 226.05 | 77.85 | 109.70 | 72.23 |
| 8 | 171.92 | 65.34 | 68.28 | 51.31 |

## 100k 记录，无长读者，2 槽，新模式

| Workers | 耗时 ms |
|---|---:|
| 1 | 64.84 |
| 2 | 92.82 |
| 4 | 103.79 |

批量合并明显减少耗时和长读者下的历史内存，但单 worker 仍最快。
因此不能宣称已经取得多 worker 吞吐扩展。slots=0 也受益，说明收益包含共享路径
只复制一次、发布/回收批次减少，不能全部归因于 fat-node 或并行执行。
无长读者时内存收益不成立：2 workers、2 槽，峰值约 17.67 → 17.88 MiB。
峰值为全进程 RSS，包含队列、请求数组、线程栈和分配器，不等于树的净内存。
长读者数据表示历史保留压力，不能推广到所有工作负载。

确定性测试覆盖两个 worker 同时准备新叶、发布前不可见、失败不发布、历史快照、
GC 及 256 片叶同时分裂并向根级联。新模式默认关闭，显式启用。
这段记录描述测量当时的范围：实现未接入原 ConCow 流水线，当时尚未推广到其它树。
当前同一独立控制器已支持四棵树，并通过 `concow-fat` 接入主程序。

下一步应评估提交、派发及合并成本，再决定是否调整线程分工。

## 复现

```sh
make test-concow-fat bench-concow-fat CC=clang++
python3 bench/concow_fat_matrix.py --materialization 0 1 --output bench/results/concow_fat_btree_materialization_arm64.csv
python3 bench/concow_fat_matrix.py --materialization 0 1 --records 100000 --updates 300000 --patterns uniform --output bench/results/concow_fat_btree_materialization_100k_arm64.csv
```
