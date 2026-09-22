# 未修改原仓库 B+Tree：worker 数量实验

2026-09-18。原仓库提交 `e487e91d31552685ea4d459223fc78375ed66591`，通过 git archive 导出；93 个受版本控制文件与提交逐字节一致。
原仓库调度器及树实现未应用任何修复，未接入 fat-node 或 GC。外部驱动只负责输入、计时和全 key 校验。

## 平台与配置

- 本机 ARM64。原 README 要求 x86-64；分别编译 ARM64 + mimalloc 和 x86-64 + 系统分配器。
- x86-64 程序通过 Rosetta 运行，mimalloc 的三个 include 由 bench/system_allocator 空替代头转到系统分配器。不是原论文 Xeon 原生运行。
- 初始 100,000 条记录，300,000 次操作，4 个提交客户端。P=1/3 对应 2/4 个 cyclic 线程，另有 monitor 和 1/2/4/8 个 worker。
- 更新负载随机修改已有 key；插入负载打乱 300,000 个新 key。随机种子 182，每次独立进程；x86 每配置预热 1 次、正式 3 次，随机化配置顺序。
- 计时包含提交、执行和等待完成，不包含建树、最终校验或析构。按返回 ticket 建参考模型，逐 key 检查原有键与新键。
- 原版没有 GC，历史分配保留到进程退出。不能直接与 GC-on 的当前版本耗时相除。

## 正确性门槛

| 平台 | 负载 | 状态 | 正式运行数 |
|---|---|---|---|
| native | insert | process_failure | 3 |
| native | update | process_failure | 3 |
| x86 | insert | timeout | 3 |
| x86 | insert | valid | 21 |
| x86 | update | incorrect | 24 |

process_failure 的返回码及错误输出保存在原始数据；incorrect 表示进程给出结果但全 key 校验失败。两者均不参与吞吐结论。
源码中的 copy_leaf 只 memcpy 128 B，而 node 为 256 B、vals 位于后半部分，未复制 value 数组；本轮保留该代码，不把修复版冒充原版。没有据此把 ARM64 的全部崩溃归因于这一处。

## Rosetta 下通过校验的插入负载

表中仅使用正式且正确的运行；范围为最小–最大值，吞吐由中位耗时计算；仅配置及其基线均为 3/3 正确时计算加速比。

| P | workers | 通过次数 | 中位耗时 ms | 范围 ms | 吞吐 Mops/s | 相对 1 worker |
|---|---|---|---|---|---|---|
| 1 | 1 | 3/3 | 557.18 | 513.78–568.88 | 0.538 | 1.00x |
| 1 | 2 | 3/3 | 569.15 | 484.09–685.46 | 0.527 | 0.98x |
| 1 | 4 | 3/3 | 1042.18 | 864.44–2345.75 | 0.288 | 0.53x |
| 1 | 8 | 3/3 | 11828.90 | 9650.64–13901.90 | 0.025 | 0.05x |
| 3 | 1 | 3/3 | 1674.58 | 1379.46–2130.45 | 0.179 | 1.00x |
| 3 | 2 | 3/3 | 1336.18 | 1250.37–2132.39 | 0.225 | 1.25x |
| 3 | 4 | 3/3 | 1595.04 | 1081.54–1650.54 | 0.188 | 1.05x |
| 3 | 8 | 0/3 | — | — | — | — |

## 能回答什么

这些数据只能回答本机 Rosetta 条件下、正确的插入负载是否受益于增加 worker。不能用失败的更新负载证明原框架快慢，也不能将 Rosetta 的调度/内存序/翻译开销外推到论文原生 x86 平台。
三次重复不构成统计显著性检验；增加 worker 的同时也增加总线程数，单 worker 仍有多个流水线线程，不等于串行执行。要复现论文的硬件扩展性，需要原生 x86-64 环境和论文的负载规模。

## 复现

```sh
mkdir -p build/pristine-worker-scaling
git archive e487e91d31552685ea4d459223fc78375ed66591 -o build/pristine-worker-scaling/source.tar
tar -xf build/pristine-worker-scaling/source.tar -C build/pristine-worker-scaling
clang++ bench/pristine_worker_scaling.cpp -o build/pristine-worker-scaling/native-p3 -Ibuild/pristine-worker-scaling -I/Users/minrain/.brew/include -L/Users/minrain/.brew/lib -Wl,-rpath,/Users/minrain/.brew/lib -lmimalloc -pthread -std=c++20 -O3 -DNDEBUG
clang++ bench/pristine_worker_scaling.cpp -o build/pristine-worker-scaling/x86-p1 -Ibuild/pristine-worker-scaling -Ibench/system_allocator -arch x86_64 -DORIGINAL_PIPES=1 -pthread -std=c++20 -O3 -DNDEBUG
clang++ bench/pristine_worker_scaling.cpp -o build/pristine-worker-scaling/x86-p3 -Ibuild/pristine-worker-scaling -Ibench/system_allocator -arch x86_64 -pthread -std=c++20 -O3 -DNDEBUG
python3 bench/pristine_worker_scaling.py
python3 bench/pristine_worker_scaling_report.py
```

[逐次数据](pristine_worker_scaling_20260918.csv) · [源文件/二进制哈希与逐次命令和输出](pristine_worker_scaling_20260918.metadata.json)
