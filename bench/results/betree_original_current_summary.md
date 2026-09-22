# 当前 BeTree 与原仓库 ConCow 对比

原始基线取自 git `e487e91d31552685ea4d459223fc78375ed66591`，使用
`conctrl::concow<betree::interface>`，3 pipes。只应用正确性修复：
`bench/btree_original_arm64.patch` 中的 ARM64 acquire/release 与 MPSC 链接同步，
以及 BeTree `copy_leaf` 将 memcpy 的 128 字节改为 `sizeof(node)`，保留所有值。
该补丁中 B+Tree 的改动对本次 BeTree 二进制不生效。基线没有 fat-node、批量
共享路径合并或 GC，因此应称为“原架构加最小正确性修复”，并非逐字节原版。

当前版本使用 `concow_fat<betree::interface>`，2 槽、批量物化开启，包含最新
checkpoint 修复。两边都关闭 GC，以排除新增 GC 带来的收益。每进程初始
100,000 键，执行 300,000 次操作，4 个提交客户端；初始 boundary 参数都是 2。
分别测均匀已有键更新和随机顺序的新键插入，种子 182，使用相同 benchmark 源码、
Apple clang 17、mimalloc、ARM64、C++20/O3/NDEBUG。

每配置一次预热、三次正式运行，交替顺序；12 配置共 36 条正式数据。每次按返回
ticket 建立最终参考值并检查全部预期键，包括未更新的基础键；36 次均零差异。
耗时包含客户端创建、提交与等待，不含最终验证。RSS 包含树历史、调度器、输入
数组、参考模型和分配器。因 GC 关闭，历史节点保留到进程结束。

## 三次中位数

| 负载 | worker 参数 | 原 ConCow ms | 当前 ms | 原 RSS MiB | 当前 RSS MiB | 加速比 | RSS 降幅 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 均匀更新 | 1 | 193.55 | 72.62 | 474.33 | 99.94 | 2.67x | 78.9% |
| 均匀更新 | 2 | 124.77 | 94.40 | 474.34 | 100.14 | 1.32x | 78.9% |
| 均匀更新 | 4 | 86.19 | 105.62 | 474.44 | 100.25 | 0.82x | 78.9% |
| 插入 | 1 | 220.09 | 74.95 | 658.17 | 100.75 | 2.94x | 84.7% |
| 插入 | 2 | 178.68 | 100.35 | 654.95 | 100.78 | 1.78x | 84.6% |
| 插入 | 4 | 163.96 | 114.25 | 658.08 | 100.97 | 1.44x | 84.7% |

新架构在本次所有配置中减少峰值内存，五组耗时更短，一组更长。原 ConCow 的
worker 从 1 增至 4 时能加速，而当前版仍是单 worker 最快。
取各自在已测配置中的最快结果，均匀更新为原版 86.19 ms 对新版 72.62 ms，
新版约快 1.19x；插入为 163.96 ms 对 74.95 ms，约快 2.19x。

相同 worker 参数不代表相同总线程数：原 ConCow 有 4 个 pipeline 线程、
1 个 monitor 和 w 个 worker；新架构有 1 个 coordinator 和 w 个 worker，
单 worker 时叶工作在 coordinator 执行。另有相同的 4 个客户端。因此这是两种
完整执行架构的对比，不是相同 CPU 预算下的纯算法消融。当前单 worker 还保留
一个未参与该批计算的池线程。

这与“同容量下新架构比 SeqCow 慢”不矛盾：此前对照是串行 SeqCow，这里对照
是原始并行 ConCow。该结果支持当前工作在这两种负载上有效减少原架构的历史
分配及总运行开销，不代表所有树、热点、GC 开启场景或论文规模均有相同比例收益。

## 复现

1. 在独立目录展开上述 git 版本，应用上述同步补丁及 BeTree 叶复制修复。
2. 同一 `bench/betree_original_compare.cpp` 编译两次：原版定义
   `ORIGINAL_BETREE`，头文件搜索路径指向独立目录；新版指向仓库根目录。
   两者均使用 `-std=c++20 -O3 -DNDEBUG -pthread` 和相同 mimalloc 安装。
3. 二进制分别命名 `build/betree_baseline_current` 与 `build/betree_latest_current`。
4. 运行 `python3 bench/betree_original_compare.py`。

原始结果：`betree_original_current_comparison.csv`。
