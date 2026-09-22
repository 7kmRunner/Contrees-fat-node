# BeTree、ART、AERT：原 ConCow 的 fat-node 第一版

2026-09-21。三棵树均已接入 `conctrl::concow` 的原固定流水线，支持
`concow <tree> --fat-slots 0/2/4/8`，以及开启或关闭 GC。
B+Tree 继续使用自己的 cyclic 调度器。本实现不调用独立的有锁 `concow_fat`。

## 当前协议与范围

入口线程按原 ticket 顺序执行。最近的结构更新已提交时，入口可以遍历完整树，
向目标叶追加一条版本记录并沿用根；槽满或需要结构改变时走原树的 staged COW。
如果前序结构更新尚未提交，则不探测可能尚未完成的孩子数组，直接走原阶段路径。
它是保守的全局准入，没有增加等待整个前序版本的入口锁，也没有把所有结构更新
交给入口串行执行；下层结构 handler 仍由原 worker 执行。

- BeTree 叶可记录已有键更新或新键插入，满槽后合并并按需分裂。
- ART/AERT 每叶一个键，fat 槽只记录该键的值历史；新键继续走前缀分叉、节点扩容。
- AERT 保留 tagged pointer、半字节节点及原分裂 handler。
- 发布记录仍先写 payload、再 release 发布 version；读者 acquire 后按快照版本筛选。
- 活叶中不复用槽位，叶和 sidecar 一起退休和回收。控制器独占树的写入。

**本版追加由有序入口执行，尚未移植 B+Tree 的 worker 预留/发布和分阶段物化协议。**
`--fat-workers` 仍仅允许 B+Tree；三棵树不会静默接受该选项。
全局准入在持续冲突写入下可能产生大量 COW 回退，不能将功能接入当作吞吐或扩展性成功。

## GC 与 checkpoint

固定流水线与 cyclic 一样使用 `epoch_reclaimer<T,true>`：monitor 独占退休队列，
处理退休、连续版本发布和回收。外部 `collect_garbage()` 只发布原子请求、等待确认，
不获取互斥锁。请求在扫描 reader slot 之前读取，确保先 unpin 再收集的调用能排空。
线程停止并 join 后，析构线程才进行最后一次回收。

BeTree 保留原 checkpoint ticket 和版本依赖。完整重建合并 fat 记录；局部重建保留
共享下层子树，GC 只回收实际替换的上层。checkpoint 也更新全局结构前沿。
上下文环的 checkpoint 槽复用时清理 flags，并携带 fat 槽配置。
由于 checkpoint 占用 ticket，**请求条数不等于最终版本号**；主程序现在等待各客户端
返回的最大实际 ticket，避免跨环后提前结束计时与回收检查。

原 BeTree 的 `copy_leaf` 只拷贝前 128 字节，遗漏值数组。已改为复制完整 node，
然后清空新叶的 sidecar 指针，保持独立所有权；slots=0 基线也包含该正确性修复。

无互斥锁指应用层写入、提交和回收路径，不是任意线程暂停下的严格 lock-free 声明；
保留论文阶段等待，也不覆盖 allocator/操作系统内部。fat 查询必须使用 root/version
回调，关闭 GC 时也不能丢弃版本信息。版本耗尽时拒绝新请求而不回绕。

## 使用

```sh
make test-native-multitree-fat test-native-ticket-wait CC=clang++
./main <dataset> concow betree -p 3 -w 2 -c 4 --gc --fat-slots 4
./main <dataset> concow art    -p 3 -w 2 -c 4 --gc --fat-slots 4
./main <dataset> concow aert   -p 3 -w 2 -c 4 --gc --fat-slots 4
```

默认槽数仍为 0。Mac ARM64 编译 main 需要移除原 Makefile 偏向 x86 的编译选项；
测试 target 已使用本机兼容的 C++20、mimalloc 参数。

## 验证

`bench/concow_native_multitree_test.cpp` 为每棵树运行：

- 24 组参数：slots=0/2/4/8 × pipes=1/3 × workers=1/2/4；每组四个客户端提交
  2400 次同键冲突、随机更新和插入，反复跨过 1024 上下文环，按实际 ticket 校验
  多个范围快照，同时运行两个 GC 收集者并固定初始快照。
- 2/4/8 槽容量边界、旧版本查询、满槽物化与 GC 排空。
- 暂停前序结构 worker 时检查后序回退及提交前缀；暂停一个收集调用者时继续写入与回收。
- 关闭 GC 的旧快照与无版本查询拒绝。
- BeTree 强制完整/局部 checkpoint，验证增量插入不丢失、共享下层与 GC。
- ART/AERT 从空树逐级扩容，然后对携带 fat 值的叶做前缀分裂。

三棵树的优化编译与 UBSan 均通过；旧 SeqCow 和独立批次控制器相关回归通过。
原生测试二进制的直接 mutex/condition-variable 引用为 0；它不是进展保证的形式证明。
ASan/TSan 尚未验收。当前没有三棵树的新吞吐/内存对照结论。

验证记录：[参数、源文件/二进制哈希、CLI 输出](../bench/results/native_multitree_fat_20260921.validation.json)。
一次 7 万次连续写入的 CLI 功能检查中，BeTree/ART/AERT 的追加分别只有 4/1024/888 次，
其余均走结构更新；这说明保守全局准入会严重限制追加率。这不是正式性能测量，下一步
应优先研究路径级就绪准入，再做内存与吞吐对照，而不是提前宣称已经取得节省。
