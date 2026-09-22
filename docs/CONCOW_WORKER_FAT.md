# B+Tree：worker 追加与分阶段物化

2026-09-17。通过 `concow btree --fat-slots 2 --fat-workers` 显式开启；默认仍为
[入口追加模式](CONCOW_CYCLIC_FAT.md)。保留原 cyclic 阶段调度、worker 池和连续提交
前缀；使用同一 [monitor 独占 GC](CONCOW_CYCLIC_GC.md)。不引入应用层 mutex 或
条件变量，依赖等待沿用用户确认的论文边界，不声称严格的任意线程暂停进展保证。

## 槽位预留与发布

入口按 ticket 顺序遍历就绪路径，仅分配槽位、写 key 并 release 发布
`DELTA_PENDING | ticket`。高位使用现有 64-bit version 的最高位，ticket 仍限制
为 UINT32_MAX；节点 256 B、delta slot 24 B、context 128 B 均不增加。
`count` 仍只由有序入口访问，不是多 writer 原子抢槽计数。

任务进入原 worker 池后，唯一负责该槽的 worker 写 value，然后 release 发布纯
ticket。不同 worker 可同时写同叶的不同槽，完成次序不要求等于 ticket 顺序。
reader acquire 加载 version，显式排除 pending，且只接受不晚于快照的记录。
monitor 只发布连续完成前缀；ticket 2 先完成不能使 ticket 1 的未完成更新可见。
活叶中不复用槽位，sidecar 随节点回收，因此没有槽位 ABA。

## 满槽物化分两步

1. 原 top-down handler 只准备结构。收集旧叶 base keys、已预留记录的 keys 和
   当前 key，去重后生成一或两个新叶的最终 keys/size，安装到复制路径。
   此时不读 pending value，也不读可能尚未填好的 source base values。
2. 内部节点完成所有指针修改后发布 `cow_ready`，后续入口可以沿新路径预留新叶
   sidecar，即使该新叶的 base value 尚未填好。源叶停止接收新预留，依赖当前
   控制器独占写入、入口 ticket 顺序与最新 root 路由；没有另设叶级锁。
3. `MATERIALIZE` 只由 worker 执行。等待 source base 就绪及其已预留槽发布，
   复制 base values，按槽位预留顺序覆盖 delta，再写本请求值，最后 release 发布
   新叶 `cow_ready`。两片分裂叶均要发布就绪。原树版本 0 视为建树已完成。
4. 新叶上的并发追加只写 sidecar，物化只写 base 的有效 value 区域，互不覆盖。
   依赖链上的下一次物化等待这个 base 完成；其 ticket 严格更晚。

**入口和 cyclic inner/exit 不执行叶值等待**：它们将 `FAT_APPEND` 与
`MATERIALIZE` 交给 worker。原 exit 交接按 ticket 分派，因此等待的前序任务已
分派，不会被后序等待者挤在分派队列之后。原内部路径遍历仍使用阶段依赖。
一次更新只有 handler 全部完成后才标记 DONE，结构先可路由不等于先可读。

## GC 与适用边界

退休仍归属于替换该节点的 ticket。source 可能被其旧追加任务和物化 worker 访问，
但 monitor 不会越过未完成 ticket；回收同时受 committed 与 reader 前沿限制。
旧快照固定 root/version，不读取后续 pending 或已发布但更晚的记录。

同一树只允许一个控制器写入；不要同时混用入口追加、SeqCow 或独立批次控制器。
初始树须由版本 0 的完整建树结果提供，运行期间不切换模式。
没有 worker 异常恢复或线程永久暂停帮助机制。allocator/OS 内部不属于应用层
无互斥锁承诺。UBSan 与符号检查不能替代 TSan 或形式化进展证明。

## 复现

```sh
make test-concow-worker-fat test-concow-cyclic-fat test-concow-cyclic-gc CC=clang++
make bench-concow-cyclic-fat bench-concow-cyclic-fat-p1 CC=clang++
bench/concow_cyclic_fat_bench --slots 4 --workers 2 --fat-workers
python3 bench/concow_worker_fat_matrix.py --output bench/results/concow_worker_fat_p3_300k_arm64.csv
python3 bench/concow_worker_fat_matrix.py --binary bench/concow_cyclic_fat_p1_bench --output bench/results/concow_worker_fat_p1_300k_arm64.csv
```

受控测试用暂停点验证同叶双 worker 重叠和逆序发布、pending 排除、旧追加暂停时
准备替换叶、新叶 base 未就绪时追加、两代物化依赖、分裂和 pinned snapshot 下 GC。
参考模型覆盖两模式各 36 组参数、两模式各 8 组满根/内部/叶级联分裂；GC 两模式
各 8 组包括多个 collector 与暂停的收集调用者。小上下文环反复复用。
执行重叠、正确性、路径复制减少和吞吐扩展性分别验收，不能相互替代。

性能测量和边界见 [同架构实验报告](../bench/results/concow_worker_fat_summary.md)。

2026-09-18：物化改为只排序增量、与已有序 base 合并；worker 填值顺序扫描 source。
没有改变发布和回收协议，也未增加节点或 context 大小。见 [优化实验](../bench/results/fat_merge_improvement_20260918.md)。
