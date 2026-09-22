# ConTree 论文与当前实现的进展保证审查

2026-09-16 原始审查；2026-09-17 更新验收口径。

用户已明确：沿用原论文不加互斥锁的阶段调度即可，同时要求启用 GC 后写入路径也
不使用互斥锁。严格的任意线程暂停进展保证不再是当前验收项。本文保留相关分析作为
术语与实现边界记录，不再要求为了该保证重写调度器。cyclic 的 GC 互斥锁已通过
[monitor 独占回收协议](CONCOW_CYCLIC_GC.md)移除。

## 论文实际说了什么

用户提供的 `3802030.pdf`：Guanhao Hou 等，*Concurrent Path-Copying Update to
Tree Structures*，SIGMOD 2026，Article 153，DOI
[10.1145/3802030](https://doi.org/10.1145/3802030)。以下依据正文，不依据早期仓库 README。

- 第 3 节，印刷页 153:6：宣称更新 non-blocking、starvation-free、order-preserving；
  immutable snapshot 上的查询 wait-free。
- 第 4.1 节，页 153:9–10，Algorithm 4：`WaitStage` 等待指定版本到达所需阶段；
  Lemma 4.1 的进展论证是依赖只从新版本指向旧版本，最早未完成更新始终具备执行条件。
- 第 4.2 节，页 153:11–12：上层串行流水线、下层独立 worker；明确指出慢 pipe
  会限制整个流水线，Principle 4.3 要求避免流水线中的昂贵 handler 和向下等待。
- 第 4.3 节，页 153:12：checkpoint 等待前序更新，并阻挡后续更新。

**审查判断**：上述“具备执行条件”与“执行该更新的线程任意暂停时，其他线程仍能
完成更新”不是同一个命题。算法未展示接替暂停 worker 的 helping 协议。
若将其论证解释为实际运行中的持续进展，至少依赖负责最早请求的线程继续获得执行机会、
handler 有限终止等条件；这是本审查对论证前提的分析，不能冒充论文显式给出的故障模型。
本审查不把局部实现反例扩大为对所有可能 ConTree 实现的否定。

## 当前代码的阻挡点

| 层次 | 代码位置 | 指定线程暂停的后果 |
|---|---|---|
| 提交 | `concow_cyclic::update` | 先保留 ticket，后初始化 context；两者之间暂停会留下版本空洞 |
| 初始化前缀 | `monitor_main_` | 只推进连续已初始化前缀；不能越过空洞，环形队列最终填满 |
| 阶段依赖 | `wait_stage`、`pipe_context::wait` | 依赖指定早期 handler/pipe 的阶段发布，没有接替执行 |
| worker 池 | `mpsc_list::push/pop` | exchange 后、链接 next 前暂停，可阻止消费者获得后续节点 |
| 提交发布 | `monitor_main_` | 单一 monitor 发布连续完成前缀；完成不同叶操作不等于可提交 |
| 快照 | `epoch_reclaimer::publish/pin` | publication sequence 为奇数期间 publisher 暂停，新 pin 会重试 |
| GC（原实现，已修复） | `epoch_reclaimer::retire/try_reclaim` | cyclic 已选择单 owner 模式；外部回收通过请求/确认，不再持有退休队列 mutex |
| fat 追加 | entry 内 `pipeline_fat_try` | 仍由原流水线依次处理入口，不是多 writer lock-free sidecar |

因此仍不能宣称 cyclic 的提交/更新完成接口具备严格 lock-free 保证。9 月 17 日的
GC 改造后，cyclic 的应用层写入及 GC 路径已不再使用互斥锁，符合当前用户确认的范围。论文的 immutable-tree 查询结论也不能直接
套用到新增的 snapshot pin/GC 包装层。分配器及系统线程调度另有其自身的进展边界。

## 可复现的证据

```sh
make test-concow-progress test-concow-cyclic-fat CC=clang++
```

`concow_progress_test` 在 ticket 1 分配后、context 发布前暂停其提交者；其他
提交者把 1024 上下文环填满，同时 monitor 继续运行至少 10000 次循环。检查：

1. submitted=1024，但 initialized=committed=0；边界提交者无法返回。
2. 恢复 ticket 1 的提交者后，所有请求完成，最终值与 GC 排空检查通过。
3. slots=0 和 slots=2 都复现，说明它不是 fat-node 独有的问题。

这是阻挡行为的确定性受控复现，**测试通过表示反例被复现，不表示通过无锁验收**。
有限运行不是无限执行的形式证明；无限停顿反例还依赖上述代码中无 helping 的等待条件。

`concow_cyclic_fat_test` 另暂停物化请求的深层 worker：相同路径拒绝探测；独立路径
在 local 模式成功追加，global 模式回退 COW。两者均不能越过暂停请求发布提交前缀，
旧快照均不暴露未来记录。该测试区分“局部工作可推进”与“更新接口可完成”。

## 保留的严格进展边界（非当前必做项）

已实现 [路径就绪准入协议](CONCOW_CYCLIC_FAT.md)：不引入新的入口自旋门，
复用原流水线的有序交接；只读取已完成构造的内部节点，未完成即回退原阶段 COW。
它减少不相关路径导致的回退，是原架构下的正确性/成本改进，**不是严格 lock-free 的实现**。

若未来重新要求严格 lock-free，仍需要以下协议有完整实现、进展论证和暂停测试，不能只更换队列：

1. **先公开完整请求，再决定全局顺序**：其他线程可获得完整参数，提交者暂停不会
   留下只有原线程才能填补的 ticket 空洞；请求描述符回收必须防止 ABA。
2. **阶段任务可帮助完成**：不能 CAS 占有任务后独占修改同一 context；需要 immutable
   输入、私有输出、唯一发布以及失败副本回收，使暂停的执行者不是唯一推进者。
3. **追加/封闭/替换共享协议**：记录发布不可暴露半条数据；物化封闭旧叶的线性化点
   必须与追加竞争，并允许帮助完成；晚到请求从当前逻辑叶重试。同版本副作用不得重复。
4. **根/版本发布可帮助完成**：连续前缀仍可保留，但完成标记与 root/version 必须由
   其他线程推进，不能停在单一 monitor 的未完成发布窗口。
5. **安全回收不阻挡写入进展**：将 reader pin、退休登记、回收及分配器的假设纳入
   明确范围，不能在真正的关键路径后面隐藏 mutex 或不可帮助的 seqlock writer。

这些属于未来更强进展保证的工作，不再作为当前开发主线。当前主线继续是 B+Tree
fat-node、原阶段调度与无互斥锁的 GC，以及独立的 worker 扩展性验证。
