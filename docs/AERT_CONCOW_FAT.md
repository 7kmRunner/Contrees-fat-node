# AERT 的有序批次 fat-node 实现

2026-09-21：原 `concow` 固定流水线另已接入 fat-node，见 [原框架第一版](NATIVE_MULTITREE_FAT.md)。下文仍描述独立有锁批次控制器。

AERT 接入既有 `lib/conctrl/concow_fat.hpp`，沿用 ART/B+Tree 的按叶分组、
worker 私有物化、协调线程合并和统一版本发布。适配在 `src/adapters/aert.hpp`，
共享路径合并在 `lib/trees/aert/fat_merge.hpp`。没有新增执行控制器。
专用 benchmark 用于可复现实验，主程序使用 `concow-fat aert` 选择该新架构。

## AERT 特有的结构处理

不同于 ART 的 Node48/256，AERT 的字节 Node16 超过容量后拆成两个四位分支。
某上半字节下面只有一个孩子时，不创建下半字节节点，而将下半字节编码在孩子指针
低五位中：一位表示存在隐式前缀，四位表示前缀值。

合并过程保留这一表示：

- 更新匹配隐式前缀时，递归处理物理节点，返回时保留指针标签。
- 插入与标签冲突时，将隐式前缀展开成 NodeH4/NodeH16，原物理孩子仍可共享。
- 字节孩子超过 16 时统一拆成上下半字节分支；单孩子下半分支继续用标签表示。
- 容量足够时只复制一次原内部节点并更新受影响孩子；不同更新的共享祖先每轮只复制一次。
- 私有节点所有权和退休列表均使用去标签的物理指针，只有树的边携带标签。

已有叶追加、槽满物化、缺失键插入沿用版本化 fat 叶。前缀分裂若需要复制旧叶，
必须带上本轮已追加且对新版本可见的值。读者固定旧根/版本，GC 在安全边界后回收；
worker 失败不发布本轮，私有节点 RAII 清理，控制器停止继续提交。

## 验证

```sh
make test-concow-aert bench-concow-aert CC=clang++
python3 bench/concow_aert_matrix.py
```

24 组 slots=0/2/4/8、workers=1/2/4、两种物化模式测试包含并发提交、ticket
参考结果、范围快照、旧根固定和 GC。专项测试包括空树、逐级与批量扩容、带未来值的
前缀分裂、worker 同时物化、失败不发布，以及标签展开后点查询和邻近键范围扫描。
通过 mimalloc 与系统分配器 UBSan；零长度前缀比较增加提前返回，避免移位 64 位。
ART、B+Tree 并行及 radix 串行回归通过。ASan/TSan 按既定决定搁置，不记为通过。

结果见 [AERT 实验报告](../bench/results/concow_aert_summary.md)。
[BeTree](BETREE_CONCOW_FAT.md) 的内部缓冲与 checkpoint 已按其独立语义接入。
