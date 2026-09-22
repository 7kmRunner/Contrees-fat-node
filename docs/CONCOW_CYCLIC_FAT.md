# B+Tree fat-node 在原 ConCow cyclic 流水线中的接入

2026-09-16：本项目回到原 cyclic 并发机制作为 B+Tree 开发主线；独立
[有序批次控制器](CONCOW_FAT_DESIGN.md)保留为带锁对照。
2026-09-17 用户确认沿用原论文的无互斥锁阶段调度，不要求严格线程暂停保证。
cyclic 的应用层写入与 GC 已采用 [monitor 独占回收协议](CONCOW_CYCLIC_GC.md)，
不使用退休队列互斥锁。术语与线程暂停边界见 [进展保证审查](CONCOW_PROGRESS_AUDIT.md)。

2026-09-17 新增显式 `--fat-workers` 的 [worker 追加与分阶段物化](CONCOW_WORKER_FAT.md)。
下文入口单 writer 协议描述默认模式；worker 模式由入口预留、worker 发布 value，
允许同叶多个 worker 重叠，并将物化的值填充推迟到 worker。

## 保留的原架构

- 客户端通过 ctxs_/stages_ 上下文环提交请求。
- P+1 个 cyclic 线程执行 entry/inner/exit；下游 worker 执行深层路径复制。
- 原 wait_stage 继续处理版本/阶段依赖；monitor 发布连续完成前缀。
- 槽满、插入后需分裂等结构更新仍通过原 top-down handlers 复制路径。
- 没有把结构更新移到管理员串行合并，也没有调用独立 concow_fat 控制器。

## 路径级准入：替代全局结构前沿

旧协议要求最近一次结构更新已经提交，才能从根探测任何叶。一个不相关的结构更新
也会关闭全树追加入口。旧协议仍可通过构造参数 `local_fat_probe=false` 启用，
基准驱动使用 `--global-probe`，便于同一二进制比较。

新默认协议利用节点头中已有的一个 padding 字节 `cow_ready`，节点仍为 256 bytes，
keys 偏移仍为 8。该标记针对内部节点的 keys、size、child pointers，不表示整棵子树
已经完成，也不表示其叶 sidecar 永远不变。

1. 新复制/分裂的内部节点初始化为未就绪。父节点可能已引用它，但它的 children
   尚未被后续 handler 填好，入口不能越过它读取未初始化指针。
2. 每个 handler 完成当前内部节点的所有修改后，以 release 发布就绪标记。
   分裂时非搜索侧 sibling 也要发布；根分裂在同一 handler 多创建一层，需额外发布新根。
3. 入口沿最新前序 root 向下。节点版本不晚于 committed 的子路径可直接读取；
   其余内部节点必须 acquire 读到就绪标记，才能读取 keys、size 和 children。
4. 遇到未就绪节点立即结束探测，回退原阶段 COW；**不能在入口等待下游节点完成**，
   避免 cyclic 线程被自身后续职责阻挡。
5. 找到完整叶且有容量，追加记录并沿用 root；槽满则走原 COW 物化。
   monitor 仍以 ticket 连续前缀发布，局部追加完成不会提前变成可见提交。

`cow_ready` 原子字节在目标平台必须 always-lock-free（编译期断言）。这只描述该
标记的原子访问，不构成整个调度器的 lock-free 保证。

## 入口顺序与同叶写入

入口依旧按 ticket 顺序执行。该顺序来自原 cyclic 交接：处理 entry(t+1) 的线程
先处理 t 的 inner/exit，acquire 读取 entry(t) 发布的 pace；跨轮次同理。
因此本轮移除了重复的 entry_done_ 自旋门，last_structural_ 的对照统计和 sidecar
单 writer 访问由已有交接保护，不增加新的全局等待。

同叶追加仍是 single-writer 协议，**没有把 count 改成原子抢槽计数，也不宣称多个
worker 同时追加同叶**。先写 key/value，再 release 发布 version；reader acquire
读取 version，仅访问非零且不晚于 snapshot 的 payload。count 只由有序入口使用。
槽位不在存活叶中复用；sidecar 随旧叶整体回收。

## 为什么追加不会越过物化而丢失

入口 t 开始结构 COW 时，新 root 已转向该更新创建的路径。后续入口从这个最新 root
开始；如果目标叶的替换尚未完成，路径上至少存在一个未就绪内部节点，探测立即回退。
当父节点就绪时，它的子指针已指向物化后的新叶或新路径，之后不会改回旧叶。

因此，后序入口不会在前序物化过程中继续向该旧叶追加。前序入口已完成的追加通过
流水线交接先于物化读取；物化合并这些记录和本次更新。其他已完成路径仍可到达其
独立叶并追加，其 sidecar 不属于正在物化的旧叶。根本身为叶的物化在入口完整完成。

这里的“停止接收”由版本路径的不可变发布保证，**不是支持任意外部 writer 的叶级
freeze/CAS 协议**。只允许当前控制器独占树的写入；独立控制器、SeqCow 或其它线程
不能同时直接调用 append_leaf_delta 修改同一树。

## 快照、GC 与限制

- 查询必须取得一致 root/version，且 reader id 不可被并发复用。fat 模式即使
  关闭 GC 也要求带 version 的查询回调。
- 纯追加不退休节点；结构更新通过 context 收集被替换节点，由 monitor 登记退休。
  GC 仍按 reader 和连续 committed 安全前沿回收；cyclic 使用 monitor 单 owner，
  外部收集者只发请求，队列访问不使用 mutex。
- 关闭 GC 时调用方负责保留/释放历史节点；版本 ticket 禁止超过 UINT32_MAX。
- 没有 worker 异常恢复协议。ASan/TSan 仍未验收；UBSan 不替代数据竞争检测。
- slots=0 与 fat 模式使用相同当前调度器；当前版本包含既有内存序和 copy_leaf 修复，
  以及节点就绪发布，不能称为完全未修改的论文原始代码。

## 使用与验证

```sh
./main <dataset.data> concow btree -p 3 -w 2 -c 4 --gc --fat-slots 4
make test-concow-cyclic-fat test-concow-progress bench-concow-cyclic-fat CC=clang++
python3 bench/concow_path_probe_matrix.py
```

仅 B+Tree cyclic 开放 fat-node，默认槽数仍是 0。新准入协议在启用 fat 时默认生效。
Mac ARM64 的 main 编译需覆盖偏向 x86 的默认 CPPFLAGS，基准 target 已可原生编译。

测试包含 P=1/3/5、workers=1/2/4、slots=0/2/4/8 每模式 36 组、共 72 组并发参考模型，
每组 4000 次混合更新/插入并多次跨过 1024 上下文环；覆盖同 key 冲突、旧快照、
并发范围扫描、GC 排空。另有两模式共 16 组满根/满内部节点/满叶的级联分裂测试，
单叶满槽、无 GC 快照、暂停前序深层 worker 时同路径回退及不同路径继续追加。
另用两个同时暂停的深层 handler 验证独立结构更新确实由两个 worker 重叠执行；
它证明执行有重叠，不证明增加 worker 后吞吐一定提升。

进展反例测试单独报告，不能算作 lock-free 验收通过。
新实验见 [路径准入结果](../bench/results/concow_path_probe_summary.md)；
[旧全局门结果](../bench/results/concow_cyclic_fat_summary.md)作为历史记录保留。
