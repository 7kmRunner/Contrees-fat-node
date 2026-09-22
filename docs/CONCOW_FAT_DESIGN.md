# ConCow fat-node 第一阶段：四棵树的有序批次架构

> 2026-09-16：本控制器使用 mutex/条件变量，保留为批量合并对照。当前主线是
> [原 cyclic B+Tree](CONCOW_CYCLIC_FAT.md)，严格无锁目标尚未达成。

本文描述当前开发主线：独立有序批次控制器及 B+Tree 适配。
[ART](ART_CONCOW_FAT.md)、[AERT](AERT_CONCOW_FAT.md) 和
[BeTree](BETREE_CONCOW_FAT.md) 已加入同一控制器。
[原 cyclic 流水线增量实现](CONCOW_CYCLIC_FAT.md) 保留作对照；主程序的
`concow btree --fat-slots 2/4/8` 仍指该原流水线实现，新架构通过
`concow-fat <btree|art|aert|betree>` 选择。

## 范围

`lib/conctrl/concow_fat.hpp` 是独立的、具备多个追加 worker 的实验控制器。
它复用各树的查询、叶物化与 Reader-version GC，不复用原 ConCow“把尚未完成的
路径交给后续阶段”的流水线执行协议。树相关差异收敛在适配 hook 和共享路径合并器。
本独立原型当时未改造 `concow.hpp` / `concow_cyclic.hpp`。其结果不能描述为原流水线的完整升级。

SeqCow 的实现和实验先归档为 [基线](SEQCOW_BASELINE.md)。构建默认值不变。

## 为什么不用直接 CAS 追加槽

旧流水线中前序 root 可能指向未构造完的后代；此时遍历整条路径本身就不安全。
即使槽位通过 CAS 独占，另一个 writer 仍可能在读完旧记录后物化并替换该叶，
使尚未完成的追加落在已被替换的物理叶上。CAS 不会自动解决这些依赖。

本实现将可并行追加与结构改动分开，先建立可验证的顺序约束，再考虑更细的并行化。

## 基线协议（parallel_materialization=false，默认）

1. 多 client 在短的提交互斥区中获得连续 ticket，并写入有界环形队列。
   初始化完成才通知 coordinator。队列空间由已提交的连续版本前沿释放。
   `update` 返回异步 ticket，调用方用 `wait_for_processing(ticket)` 等待生效；
   不宣称 `update` 返回时就已可读。32-bit 版本用尽时拒绝新提交。
2. coordinator 从完成构造的 root 出发，按 ticket 顺序规划最多 256 个操作。
   一个批次内按物理叶分组，容量预留只存在于固定大小的临时哈希表。
   碰到第一个容量不足的操作就停止规划，不能跳过它继续规划更晚的 ticket。
3. 一组仅由一个 worker 执行，组内保持 ticket 顺序；不同叶的组可以并行。
   sidecar 的 `count` 仍只有一个独占 writer，不需要改为多 writer 预留计数。
   key/value 初始化后 release 发布 version，reader acquire 读到 version 后才读 payload。
   小于 `parallel_min_groups`（默认 16）个叶分组或配置只有一个 worker 时
   由 coordinator 直接执行；单叶批次始终直接执行。另记录实际派发到并行批次的更新数。
4. 所有组完成后才发布 `{root, 最后完成的 ticket}`，然后推动 committed 前沿。
   已发布 payload 但尚未发布整个批次时，旧 reader 仍通过版本过滤读旧值。
   root/version 通过既有 reclaimer 的一致快照协议发布，GC 关闭时也启用该协议。
5. 容量不足的操作进入独占结构阶段。所有追加 worker 已完成，coordinator
   物化叶的全部可见记录，再调用 COW 路径；可以分裂叶和内部节点。
   新 root 完成后退休被替换节点并发布版本，然后才允许规划下一批。
6. worker 异常使当前批次不提交，控制器进入失败状态；等待者与后续提交收到异常。
   不重放已写入的半批次记录。此前提交的快照仍通过版本过滤保持正确。
   析构时正常路径排空所有已接受请求，再停止 worker；当前可达树由调用者拥有。

公开查询接口只接受 `(root, version)` 回调。每个同时活跃的 reader 必须使用独立
reader id，不允许嵌套复用同一 id，也不允许未 pin 就持有 root 跨更新访问。
控制器要求独占一个新建、版本为 0 的树；不能将同一树交给另一个 writer 控制器，
也不能将失败后含有未提交记录的树重新当作新树使用。

## 并行物化模式（显式启用）

`options.parallel_materialization=true` / benchmark `--parallel-materialization`
启用新协议，旧模式保留为同一二进制的对照。

1. 每个原始物理叶最多预留“剩余槽数 + 1”个操作。最后一个操作负责物化；
   遇到该叶的下一条操作才截断连续前缀，重新规划必须等到本轮发布之后。
2. 一个 worker 独占一个叶组，先依序追加，再用本组最后一条 ticket 物化。
   其它叶可以同时追加或物化。每组最多产生两片私有新叶，原叶的路由不变。
3. 全部 worker 完成后，协调线程将替换项按路由 key 排序，用
   `lib/trees/btree/fat_merge.hpp` 自底向上合并。未改动子树直接共享；
   同一个旧内部节点只替换一次。每个旧孩子最多变成两个孩子，内部节点最多
   临时容纳 32 个孩子，必要时均分为两个节点并把分裂向上传递；根可增高。
4. 全部新路径构造完成，才登记退休节点、转移新节点所有权，并发布本轮最后
   ticket 对应的 root/version。一次发布包含完整连续前缀，不提供中间 ticket 的
   独立根；`wait_for_processing` 允许返回时提交前沿已经超过所等 ticket。
5. 旧读者仍使用旧根和版本过滤；新根只会在整轮完成后被读者获得。
   退休版本统一为本轮发布 ticket，外部 GC 的 committed 前沿在发布前不会越过它。

worker 结果以 RAII 拥有私有新叶，合并器拥有私有新内部节点。worker 失败时
本轮不发布，私有结果最迟随控制器销毁释放，已追加未来记录保持不可见。
合并阶段不写旧节点。临时叶数量至多为叶分组数的两倍，内部路径内存受
批次大小与树高约束；没有随整棵树增长的常驻锁或替换映射。

这是“跨叶并行准备 + 单协调线程合并”，仍有轮末屏障。同叶完成一次物化后
必须进入下一轮；热点仍会截短批次。slots=0 也支持该模式：每叶每轮一个物化，
共享路径可合并，因而不能把它的收益全部归因于 fat-node 或多 worker。

## 基线核心不变量及理由

- **同叶互斥**：固定哈希表将物理叶映射到唯一 group；原子任务索引只把 group
  分配给一个 worker。组内链表按输入顺序追加，无法重复预留同一槽。
- **不丢更新**：容量耗尽之前的连续 ticket 前缀全部完成后才执行物化；
  物化期间没有追加。后续 writer 从完成的新 root 重新查找叶，因此不会写旧叶。
- **结构安全**：并行阶段只改 append-only sidecar，root/children/keys 不改动。
  结构阶段只有 coordinator，因此不存在未构造好的子指针跨 worker 被遍历。
- **快照安全**：只发布没有空洞的已完成前缀；未来版本的记录不能被旧 reader 读取。
  正确性是“已等待 ticket 的生效顺序 + reader 的一致提交快照”，不是同步 update API。
- **GC 安全**：物化前 worker 全部退出当前批次，之后的批次只使用新 root。
  退休节点仅在 writer 提交前沿与 reader 安全前沿都越过退休版本时释放，
  sidecar 与物理叶共同回收。外部 collect 使用相同的 committed 前沿。
- **内存有界**：队列默认 4096 条，批次默认 256 条；哈希表、group 和任务数组
  预留后复用，大小取决于队列/批次，不随树节点数增长；没有每节点常驻锁。
  长 reader 仍可保留任意多历史，这是快照语义的必要成本，本实现只减少该成本。

## 正确的实验 0 槽对照

新增随机参考模型测试发现原 B+Tree 的现有 key 更新分支调用 `copy_leaf`，
但该函数只复制 128-byte 头部，没有复制其它 value，随后仅赋值本次更新槽。
因此不将这个分支作为正确性等价的实验对照，也没有顺带修改原调度器的该分支。

实验控制器的 slots=0 禁止所有 append，每次操作都走完整物化 COW；它与 2/4/8
模式使用相同控制器、队列和正确的物化逻辑。`handle_init_cow` 是抽出的结构入口，
原 `handle_init` 保留原 fast-path 判断并调用同一结构入口，SeqCow 行为不变。
**实验 0 槽既不是原 ConCow，也不是 SeqCow 0 槽，不能混用这些标签。**

## 验证与限制

`make test-concow-fat CC=clang++` 覆盖：

- 1/2/4 worker、0/2/4/8 槽，4 个并发提交者，每种组合 2000 次混合更新/插入；
- 按返回 ticket 排序的参考 map，同时验证并发读者记录的完整范围快照；
- 31 条队列和 13 条批次，反复跨环形队列边界；同 key 热点、结构分裂；
- pinned reader 保留历史，释放 pin 后 GC 计数完全清空；
- 控制门明确观测到不同叶的两个 worker 重叠执行（不以吞吐或 group 数冒充证据）；
- 控制门让最后一个槽写入后暂停，验证未来记录不可见、物化必须等待；
- GC 开/关的版本过滤、半批次成功后另一 worker 抛异常、析构排空与无效配置；
- 上述参考 map 矩阵同时覆盖两种物化模式；
- 控制门确认两个 worker 同时持有已准备新叶，发布前新叶不可达，GC 不退休旧叶；
- 并行物化中途抛异常，私有叶销毁，已提交根/版本不变；
- 同轮 256 片叶全部分裂，两个内部层和根级联分裂，精确检查 273 个旧节点
  只退休一次，并验证完整范围扫描及旧版本。

优化构建和 UBSan（mimalloc、系统分配器）通过；四棵树已有功能测试通过。
ASan/TSan 按用户要求暂时搁置，尚未完成该项验收，不能宣称没有数据竞争。
测试支持上述协议，但不是形式化证明。

## 历史性能边界

独占结构阶段会等待整个批次，少量 group 的频繁派发仍可能比单 writer 慢。
首轮测量同时报告无长 reader 与持续 pinned reader，避免只挑选有利情形。
初版结果见 `bench/results/concow_fat_btree_summary.md`，派发优化后的
432 条对照见 `bench/results/concow_fat_dispatch_summary.md`；先验证内存收益及其代价，
再决定是否减少屏障或调整批次策略。四棵树的第一阶段适配已经完成。

最新并行物化对照与后续判断见
[并行物化实验](../bench/results/concow_fat_materialization_summary.md)。
