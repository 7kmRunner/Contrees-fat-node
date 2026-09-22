# BeTree 的有序批次 fat-node 实现

2026-09-21：原 `concow` 固定流水线另已接入 fat-node，见 [原框架第一版](NATIVE_MULTITREE_FAT.md)。下文仍描述独立有锁批次控制器。

BeTree 复用 `lib/conctrl/concow_fat.hpp` 的有序批次、按叶分组、worker 私有物化、
统一合并和版本发布。适配位于 `src/adapters/betree.hpp`，树结构合并位于
`lib/trees/betree/fat_merge.hpp`。专用 benchmark 用于可复现实验，主程序使用
`concow-fat betree` 选择这个独立控制器。

## 与 B+Tree 合并的区别

BeTree 的上层节点、elastic boundary 和下层节点有不同职责。普通内部节点可以
将多个孩子分裂一起合并，并让共享祖先每轮只复制一次。Boundary 不能直接增加
分裂后的孩子：合并器在 boundary 下创建新的 elastic 根，将左右孩子包装在其中，
并提升该子树的高度。这与原 BeTree 的逐次路径复制语义一致。

当 elastic 高度超过 `E` 时，本轮先完成共享路径合并，再立即 checkpoint。完整
checkpoint 物化所有可见 fat 记录；部分 checkpoint 只重建上层外壳并继续共享
高度为零的下层子树。合并前的旧路径和 checkpoint 前的临时外壳分别加入同一退休
批次，GC 等旧读者和 writer 安全边界后回收。部分 checkpoint 的回收遍历在共享
下层停止，避免释放新根仍引用的节点。

每个叶组仍由一个 worker 独占，sidecar 的非原子 `count` 不由读者读取。记录先写
key/value，再以 release 写版本；查询以 acquire 读取版本。worker 失败时不发布
本轮，私有叶和内部节点由 RAII 清理。

## 验证与复现

```sh
make test-concow-betree bench-concow-betree CC=clang++
python3 bench/concow_betree_matrix.py
```

24 组 slots=0/2/4/8、workers=1/2/4、串行/并行物化配置覆盖并发提交、ticket
参考模型、版本快照、范围查询、GC 和异常不发布。专项测试覆盖多叶同时物化、两层
内部节点和根的级联分裂，以及 Boundary 子树增长后触发 full/partial checkpoint；
后者覆盖串行物化与批量合并两种模式，直接断言重建发生一次及 full/partial 类型，
验证旧快照、最终 369 个键、延迟回收，并比较未更新叶的物理地址：部分重建保留，
完整重建替换；GC 排空后再次验证共享叶的基础值和历史值。

mimalloc 与系统分配器 UBSan 通过；B+Tree、ART、AERT 并行测试和四棵树串行
fat-node 回归通过。ASan/TSan 按既定决定搁置，不记为通过。实验结果见
[BeTree checkpoint 修复与同容量实验](../bench/results/concow_betree_checkpoint_fixed_summary.md)。
