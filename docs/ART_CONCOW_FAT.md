# ART 的有序批次 fat-node 实现

2026-09-21：原 `concow` 固定流水线另已接入 fat-node，见 [原框架第一版](NATIVE_MULTITREE_FAT.md)。下文仍描述独立有锁批次控制器。

ART 复用 `lib/conctrl/concow_fat.hpp`，没有新建一套调度器。树相关实现位于
`lib/trees/art/fat_merge.hpp`，接口在 `src/adapters/art.hpp`。
专用 benchmark 用于可复现实验；主程序使用 `concow-fat art` 选择该执行协议。

## 更新与结构合并

协调线程按提交 ticket 取连续批次，将已有键按物理叶分组，将缺失键按完整键分组。
每组由一个 worker 顺序追加版本记录；不同组可并行。槽满时准备私有叶，
每轮每组至多物化一次；同一缺失键的后续更新留到重新路由后的下一轮。
ART 每叶只有一个键，新增键必须创建叶，不能用旧叶的历史槽吸收插入。

worker 完成后，协调线程将替换叶按键排序，递归合并受影响路径。共享祖先在该轮
只复制一次，各自独有的路径仍需要复制，并不是整个批次只分配一个节点。
压缩前缀发生分叉时创建分支并调整后代前缀；容量不足时按 Node4/16/48/256 扩容。
内部节点容量足够时直接复制一次、替换或插入受影响孩子，避免逐次重建全部 256 个槽。

全部结构完成后发布一致的 `{root, version}`，再推进连续提交前沿。
旧根和叶历史继续服务固定版本的读者，GC 等读者与 writer 安全边界后回收。
前缀分裂复制叶时必须物化本轮已追加的值，避免结构变化丢失更新。
worker 失败时不发布该轮，私有节点由 RAII 释放，控制器进入失败状态；不提供恢复提交。

## 验证与复现

```sh
make test-concow-art bench-concow-art CC=clang++
python3 bench/concow_art_matrix.py --output bench/results/concow_art_tuned_arm64.csv
```

测试覆盖 slots 0/2/4/8、workers 1/2/4、两种物化模式共 24 组，
并发提交、ticket 顺序参考结果、范围快照、旧根固定与 GC。
专门覆盖空树、节点逐级扩容、单轮批量扩容、带未来追加值的前缀分裂，
以及两个 worker 物化重叠和异常注入时不发布未完成版本。
同时修复 ART 零长度前缀比较的移位 64 位未定义行为。

测量与限制见 [实验报告](../bench/results/concow_art_summary.md)。
[AERT 已适配 tagged pointer](AERT_CONCOW_FAT.md)，[BeTree](BETREE_CONCOW_FAT.md)
也已完成 elastic boundary 与 checkpoint 适配。
