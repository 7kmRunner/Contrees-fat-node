# 后续工作路线图

本文区分“已经实现并验证”和“尚未实现的计划”，用于后续开发、实验复现和
代码审查。除非对应条目标记为完成，否则不能把设计设想当作当前功能。

## 当前主目标（2026-09-17 用户确认）

沿用原论文不加互斥锁的版本/阶段调度，以 fat-node 降低 B+Tree 路径复制成本；
启用 GC 后应用层写入路径也不得使用互斥锁。不要求任意线程暂停时的严格 lock-free，
不为此重写原执行器。先做 B+Tree；独立有序批次控制器只作带锁对照。

路径级就绪准入已实现；cyclic GC 已改为 monitor 独占退休队列，外部通过原子请求/
确认触发回收，无退休队列互斥锁。见 [GC 协议](docs/CONCOW_CYCLIC_GC.md)。
已实现显式 `--fat-workers`：有序入口预留槽、worker 发布、结构准备与值物化分离，
同叶重叠、逆序发布、连续物化与 GC 已有受控测试，见 [worker 协议](docs/CONCOW_WORKER_FAT.md)。
严格进展审查保留为术语边界，不再是未完成验收项。下一重点仍是同架构 fat 开关的
成本与吞吐，以及 1/2/4 workers 的扩展性；已有实验未证明稳定扩展，不能提前标记完成。

## 当前状态

| 模块 | 状态 | 当前范围 |
|---|---|---|
| Reader-version GC | 已完成 | SeqCow、ConCow 及四种树的退休节点回收；BeTree checkpoint 区分完整重建与共享下层子树 |
| B+Tree fat node | 已完成第一阶段 | SeqCow、叶节点、槽数 2/4/8、版本化查询、GC；内存优先默认值为 2 |
| BeTree fat node | 已完成第一阶段 | SeqCow、叶节点、槽数 2/4/8、版本化查询、GC、full/partial checkpoint；内存优先默认值为 2 |
| ART fat node | 已实现第一阶段 | SeqCow、2 槽内联、4/8 按需 overflow、版本查询/扫描、GC；ASan/TSan 待补 |
| AERT fat node | 已实现第一阶段 | 保留 tagged pointer；功能同 ART，独立测试/内存矩阵；ASan/TSan 待补 |
| ConCow fat node | 四棵树的独立有序批次第一阶段已完成 | B+Tree、ART、AERT、BeTree 均支持并行叶物化、共享路径合并、版本查询与 GC；BeTree 含 checkpoint；原 B+Tree cyclic 与其他三树固定流水线均已接入 fat-node，含 GC 的应用层路径无互斥锁；其他三树为保守入口追加第一版，扩展性待验证 |

## 约束和不变量

后续实现需要继续满足以下约束：

1. 不改变 `--fat-slots 0` 选择的原仓库更新路径，也不顺带修复与目标无关的
   原有问题。
2. Reader 必须先取得一致的 `{root, version}` 快照；读取 sidecar 时只能选择
   `record.version <= snapshot_version` 的最新记录。
3. Writer 写完 key/value 后，最后以 release 操作发布 version；Reader 先以
   acquire 操作读取 version，不能读取尚未发布的 payload。
4. 物理节点及其 sidecar 具有同一所有权。只有节点已经从新版本不可达，并且
   退休版本不晚于 Reader/Writer 安全边界时，GC 才能一起释放它们。
5. 新版本提交前必须合并目标叶的基础内容、可见 sidecar 记录和本次更新。
   worker 模式允许先完成结构准备，但 pending 值与未就绪 base 不得提前对读者可见。
6. 每棵树单独选默认槽数。不能因为 B+Tree/BeTree 都选择 2，就直接假定
   ART/AERT 也应使用 2。

## ART / AERT 第一阶段已落地

设计、验收覆盖、实际内存数字及尚未完成的验证见
[radix_fat_design.md](bench/results/radix_fat_design.md)。两棵树均保持 64-byte 叶布局，
默认暂保留 slot 0，推荐手动尝试 slot 2；已完成 mimalloc 100k/1m 本地矩阵（见 `bench/results/mac_mimalloc_validation.md`），
已补有限 Zipfian 的 100k/1m 矩阵，未完成论文规模跨平台调优，
不把这次系统分配器小规模结果当成完整验收。ASan/TSan 重试仍被运行环境阻碍，按用户要求暂时搁置，仍属于未完成验收项。
ART/AERT 原有范围扫描均已接入版本选择，修正文档此前“没有范围扫描接口”的判断。

## ConCow 第一阶段：独立 B+Tree 原型

已实现 `lib/conctrl/concow_fat.hpp`，采用有序批次与按叶分组，避免每节点常驻锁。
不同叶可并行追加和物化；同一叶按提交顺序处理；轮末由协调线程合并共享路径。
新模式显式启用，默认保留旧模式作为对照。
它是新的实验执行协议，不是原 ConCow 流水线的 fat 开关。

- SeqCow 回退基线：`docs/SEQCOW_BASELINE.md`。
- 设计、顺序约束、所有权与验证：`docs/CONCOW_FAT_DESIGN.md`。
- 测试/内存与时间矩阵：`make test-concow-fat bench-concow-fat`，
  `bench/results/concow_fat_btree_summary.md`。
- 原 `concow btree --fat-slots` 现已开放，见 `docs/CONCOW_CYCLIC_FAT.md`；TSan 按用户决定暂时搁置，不记为通过。

小批次派发阈值已加入并完成 432 条对照（`bench/results/concow_fat_dispatch_summary.md`）；
单 worker 仍最快，不能声称已实现多 worker 吞吐扩展。并行物化及共享路径合并已实现，结果见
`bench/results/concow_fat_materialization_summary.md`。分段计时及 153 条计时/派发门槛
测量已完成，见 `bench/results/concow_fat_profile_summary.md`：并行轮交接成本突出，
热点的频繁版本发布也很昂贵；提高派发门槛主要通过减少并行量改善耗时。
当前以原 cyclic B+Tree 为主线，独立有序批次架构保留作带锁对照。
原 cyclic B+Tree 第一版的 96 条同架构对照仍保留；持续提交下全局结构前沿使追加率偏低。
ART 已接入同一独立控制器，包含缺失键插入、压缩前缀分裂和节点扩容，见
[ART 设计](docs/ART_CONCOW_FAT.md) 与 [测量报告](bench/results/concow_art_summary.md)。
AERT 已保留 tagged pointer 并接入同一控制器，见 [AERT 设计](docs/AERT_CONCOW_FAT.md)。
BeTree 已保留 elastic boundary，并在批量合并后执行 reader-safe checkpoint，见
[BeTree 设计](docs/BETREE_CONCOW_FAT.md)。四棵树的第一阶段和 `concow-fat`
主程序入口已经完成；跨树推进暂停，优先解决 B+Tree 的协议与进展性。不能将独立控制器的 slots=0
强制物化对照称为原 ConCow 的性能数据，也不能把多个 worker 可并行执行
当成吞吐必然随 worker 数增长。

## 后续阶段四：GC 与缓冲压缩

在正确性稳定后，再探索更细粒度的空间回收：

1. 利用 `oldest_active_version` 判断哪些 sidecar 历史记录已不可能被 Reader
   访问。
2. 不在仍被 Reader 访问的 sidecar 上原地搬移记录；通过安全的替换、物化或
   copy-and-publish 完成压缩。
3. 比较“槽满才物化”和“根据无效记录比例提前压缩”两种策略的峰值 RSS、
   live bytes、分配流量与吞吐。
4. 研究按叶热点自适应容量：读多写少叶保持 0/2 槽，热点写叶使用 4/8 槽。

## 实验与复现待办

1. 在正式 x86-64/AES-NI 环境补跑论文规模或可承受的等比例规模。
2. SeqCow 单 writer 调优与 ConCow 多 worker 调优分开报告，不能用后者解释
   前者的结果。
3. 每组测试保留原始 CSV、编译器/架构、参数、warm-up 次数和运行顺序；报告
   同时给出峰值 RSS、最终 reachable live bytes、sidecar bytes 和累计退休
   字节。
4. 读多写少场景必须保留 slot 0，因为现有结果已经表明 sidecar 可能使峰值
   RSS 上升。
5. 最终提交建议拆分为：GC 基础设施、B+Tree fat node、BeTree fat node、
   ART fat node、AERT fat node、ConCow fat node/压缩策略，方便独立审查和回退。

## 已有证据入口

- 总体用法与当前限制：`README.md`
- GC 测试说明：`bench/README.md`
- B+Tree 调优报告：`bench/results/fat_slots_arm64_summary.md`
- BeTree 调优报告：`bench/results/betree_fat_slots_arm64_summary.md`
- BeTree 原始矩阵：`bench/results/betree_fat_slots_arm64.csv`
- 四棵树新架构总结：`bench/results/concow_fat_multitree_summary.md`
- ART/AERT/BeTree 新架构报告：`bench/results/concow_art_summary.md`、
  `bench/results/concow_aert_summary.md`、`bench/results/concow_betree_summary.md`

## 2026-09-21：其他三树原流水线接入

BeTree、ART、AERT 已支持原 `concow` 的入口 fat 追加、原阶段 COW、版本快照与
monitor 独占 GC；BeTree 完整/局部 checkpoint 已验证。见 [实现范围](docs/NATIVE_MULTITREE_FAT.md)。
后续仍需路径级准入、worker 追加协议及同架构内存/吞吐对照，不能直接套用 B+Tree 的实验结果。
