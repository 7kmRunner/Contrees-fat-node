# 原仓库 B+Tree 与最新 fat-node 控制器对照

## 比较对象与条件

- 原仓库提交：`e487e91d31552685ea4d459223fc78375ed66591`，通过 git archive 导出。
- 原版使用 `concow_cyclic<btree::interface, 3>`：4 个流水线线程，另有 1/2/4 个后续处理 worker 和 monitor。
- 最新版使用 `concow_fat`，2 槽、parallel_materialization=true、workers=1；协调线程直接执行叶更新和路径合并。两个实现的 worker 含义、总线程数不同。
- Mac ARM64、Apple clang、mimalloc，同一驱动、`-std=c++20 -O3 -DNDEBUG`，同一台机器顺序执行。
- 两边 GC 都关闭；原仓库没有 GC。没有并发查询/长读者，但所有历史分配保留到进程结束。这不是 GC 开启时的峰值预测。
- 初始 100,000 条记录、300,000 次操作、4 个提交客户端。修改负载为 uniform 已有 key；插入负载为打乱顺序的全新 key。
- 随机种子 182，同一输入请求集合；多客户端的实际提交顺序可不同。按各次返回的 ticket 生成期望值，最终逐 key 验证，包括未更新的初始 key。
- 每配置一次预热、三次独立进程测量；配置顺序交替，表中均为中位数。时间包括客户端提交、执行和等待全部完成，不包括建树、正确性检查或析构。
- 原版保留默认 65,536 上下文环，最新版队列为 4,096、批次 256。比较的是完整实现，不是单独测某个组件。

## 未修改原版的结果

两种负载 × 三种 worker 配置 × 三次测量，共 18 次均 SIGSEGV（退出码 -11），没有有效耗时。预热也失败。
小规模 UBSan 定位到 `lib/trees/btree/update_cow.hpp:51` 通过不对齐的无效子节点指针访问 node。
这不能证明原版在其原目标 x86 环境也有同样崩溃，不能拿失败时间计算加速比。

为获得能校验的对照，仅在另一隔离副本应用 `bench/btree_original_arm64.patch`：

1. common.hpp 的 volatile + atomic_signal_fence 改为真正的 acquire/release 原子读写。signal_fence 本身不建立线程间同步。
2. mpsc_list.hpp 的 next 指针发布和读取也改为原子操作。
3. copy_leaf 复制完整 sizeof(node)，补全原来遗漏的 value 数组。

原版源码导出目录和工作区生产实现均未被此补丁修改。补丁后的对象称为“最小修复原 ConCow”，不是未修改原版。未分别消融各同步修改，不将具体崩溃归因于其中单独一行。

## 修改已有 key

| 实现 | 配置 worker | 耗时 ms | 进程峰值 MiB | 全 key 校验 |
|---|---:|---:|---:|---|
| 最小修复原 ConCow | 1 | 217.23 | 474.31 | 3/3 通过 |
| 最小修复原 ConCow | 2 | 217.32 | 474.34 | 3/3 通过 |
| 最小修复原 ConCow | 4 | 209.65 | 474.45 | 3/3 通过 |
| 最新 fat-node + 批量合并 | 1 | 68.68 | 99.92 | 3/3 通过 |

相对本轮最小修复原 ConCow 最快配置：吞吐约 3.05 倍，峰值下降约 78.9%。

## 插入新 key

| 实现 | 配置 worker | 耗时 ms | 进程峰值 MiB | 全 key 校验 |
|---|---:|---:|---:|---|
| 最小修复原 ConCow | 1 | 235.32 | 486.59 | 3/3 通过 |
| 最小修复原 ConCow | 2 | 209.39 | 486.63 | 3/3 通过 |
| 最小修复原 ConCow | 4 | 226.88 | 486.70 | 3/3 通过 |
| 最新 fat-node + 批量合并 | 1 | 71.11 | 99.61 | 3/3 通过 |

相对本轮最小修复原 ConCow 最快配置：吞吐约 2.94 倍，峰值下降约 79.5%。

## 结论与边界

在这台 Mac、这些负载和无 GC 条件下，最新版比经过最小修复的原流水线更快，历史分配造成的峰值也更低。该收益包含 fat-node、共享路径批量合并、发布策略和调度差异，不能全部归因于单线程，不能外推为原 x86 平台的加速比。
峰值 RSS 包含上下文环、请求数组、线程及分配器保留内存；最终可达树的 live_bytes 另见 CSV，不能将峰值下降解读为净树体积同比下降。
短实验仅三次重复，原版不同 worker 之间的小差异不足以得出最佳线程数的普遍结论。未修改原版失败，无法给出其正确结果下的直接加速比。

## 复现与文件

驱动：`bench/btree_original_compare.cpp`；矩阵：`bench/btree_original_compare.py`。
CSV：`btree_original_comparison.csv`（原样运行，含失败）和 `btree_patched_comparison.csv`（修复版与最新版）。

```sh
git archive e487e91d31552685ea4d459223fc78375ed66591 | tar -x -C <original-dir>
cp -R <original-dir> <patched-dir>
patch -d <patched-dir> -p1 < bench/btree_original_arm64.patch
# 三次编译使用相同源文件和公共参数；将 <prefix> 设为本机 mimalloc 前缀。
# 原版: -DORIGINAL_BTREE -I<original-dir>  -o build/btree_original_compare
# 修复: -DORIGINAL_BTREE -I<patched-dir>   -o build/btree_patched_compare
# 最新: -I.                               -o build/btree_latest_compare
# 公共参数:
# clang++ bench/btree_original_compare.cpp ... -I<prefix>/include -L<prefix>/lib
#   -Wl,-rpath,<prefix>/lib -lmimalloc -pthread -std=c++20 -O3 -DNDEBUG
python3 bench/btree_original_compare.py
python3 bench/btree_original_compare.py --patched
```
