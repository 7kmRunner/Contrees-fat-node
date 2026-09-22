# cyclic 写入路径的无互斥锁 GC

2026-09-17。按用户确认的验收口径：沿用原论文的版本/阶段等待，不要求线程任意
暂停时的严格 lock-free；提交、写入、提交发布及 GC 的应用层路径不使用互斥锁。
改动针对当前 B+Tree 使用的 `concow_cyclic`，并不把独立批量控制器算作无锁实现。

## 所有权协议

此前 monitor 登记退休节点，外部 `collect_garbage()` 也直接操作同一队列，需要
`retired_mutex_`。现在使用 `epoch_reclaimer<T, true>`：

1. 更新 worker 仍仅向自己 context 的退休列表追加，不访问 GC 队列。
2. monitor 沿连续完成前缀接收退休列表，发布 root/version，然后执行回收。
   运行期间只有 monitor 能访问退休队列，模板实例中不含 mutex，也不调用锁函数。
3. 外部 `collect_garbage()` 用原子计数提交请求，等待 monitor 完成至少一次覆盖
   该请求的回收检查。外部调用者不取得队列所有权，不持有任何锁；暂停它不会阻挡写入。
4. monitor 先 acquire 获取请求边界，再扫描 reader slots；最后 release 确认该边界。
   扫描开始后到达的请求留到下一轮，不能误确认尚未覆盖的 unpin。
5. 即使没有新提交，monitor 也处理回收请求；快照退出后无需靠额外写入触发回收。
6. 关闭时先 join monitor 和工作线程，然后析构线程执行最后一次回收。运行期所有权
   此时已结束；与原 API 一样，析构不能和其它线程的调用并发进行。

这是对队列访问所有权的调整，没有用原子自旋锁替换 mutex。原有结构复制、叶物化
仍由 pipeline/worker 执行；没有转给 monitor 串行处理。monitor 仍承担原来的版本发布
职责，GC 请求者等待的是回收结果，不是写入者等待 GC 请求者释放某个资源。

## 安全边界不变

只回收退休版本不晚于 `min(最老活跃读者版本, 连续提交版本)` 的节点。快照 pin 的
发布序列验证、reader slot、fat 记录版本过滤及 sidecar 随叶回收均保持原协议。
有长快照时 `collect_garbage()` 完成的是一次安全检查，不承诺把仍被快照使用的节点释放。

显式请求计数及 root 指针使用平台 always-lock-free 原子类型（编译期检查）。本文的
无互斥锁范围是应用层调度和 GC 协议，不承诺分配器、标准库和操作系统内部完全无锁。
原论文阶段等待、单 monitor、读者发布重试等边界仍保留，详见
[进展审查](CONCOW_PROGRESS_AUDIT.md)；严格线程暂停保证不再作为本轮验收项。

默认 `epoch_reclaimer<T>` 仍保护其它控制器的共享访问。只有确实保证单线程所有权
的控制器才能使用第二个模板参数 `true`，不能单独切换模板而继续允许外部直接回收。

## 验证

```sh
make test-concow-cyclic-gc test-concow-cyclic-fat test-concow-progress test-concow-fat CC=clang++
make -B test-fat-btree CC=clang++
```

新增 8 组测试覆盖 P=1/3、slots=0/2/4/8：两提交者跨环写入、三个并发回收请求者、
当前快照读取、长期持有旧快照，以及领取回收请求号后暂停的调用者。全部写入和其它
回收请求必须在暂停调用者恢复前完成；旧快照持有期间不释放退休节点，退出后空闲
回收排空；测试记录释放线程，确认运行期实际释放只发生在 monitor。

原 cyclic 36 组并发参考模型、8 组级联分裂及暂停/重叠场景，和 SeqCow、独立批次
B+Tree 回归均通过。系统分配器 UBSan 验证新 GC 与 cyclic fat 两套测试。
ASan/TSan 仍未验收，UBSan 不替代数据竞争检测。

符号检查及命令记录见 [本轮验证记录](../bench/results/concow_cyclic_gc_validation.json)。
9 月 16 日的路径准入性能报告对应改造前 GC，保留为历史测量，不冒充本轮 GC 的性能数据。
