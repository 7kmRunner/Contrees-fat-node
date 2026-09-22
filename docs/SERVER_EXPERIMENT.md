# x86 服务器第一轮实验

本轮不修改算法。先检查四棵树正确性，再对当前 B+Tree cyclic 调度器做
fat 开关和 worker 数对照。不是未修改的论文原版复现，也不包含其他三棵树的性能矩阵。

## 依赖与执行

需要 Linux、支持 C++20 的 g++（建议 12 或更新）、Python 3、mimalloc 开发库。
Ubuntu/Debian 可安装 `build-essential python3 libmimalloc-dev`。无需 Python 第三方包或外部数据集。
在仓库目录执行：

```bash
git pull --ff-only
python3 bench/server_experiment.py
```

默认 10 万初始记录、10 万次更新、4 个客户端；P=1/3，workers=1/2/4/8；
uniform/hot；固定/不固定初始快照；slots=0 基线及 slots=2/4/8 的 entry/worker 模式。
共 224 个配置，每组预热一次、正式三次，总计 896 个独立进程，随机顺序、串行执行。
每个性能进程最多 120 秒；完整运行时间取决于服务器、超时和编译时间。
所有性能样本均启用 GC，pinned=1 只表示保留初始旧快照。
不绑核，记录可用 CPU affinity、CPU/内存信息、cgroup 限额与开始时负载。
共享服务器尽量选择空闲时运行，不要并行启动多份实验。

环境冒烟可先用下面的命令；它仍运行全部正确性检查，但只测小型性能子集，不能当正式性能结论：

```bash
python3 bench/server_experiment.py --smoke --records 1000 --updates 2000 --runs 1 --warmups 0
```

非系统路径的 mimalloc 可通过 `--extra-flags '-I... -L... -Wl,-rpath,...'` 指定。
支持 `--compiler`、`--workers`、`--runs`、`--warmups`、`--records`、`--updates` 和 `--timeout`。

## 正确性门槛

优化编译运行 cyclic、worker 发布协议、GC、主程序 ticket 等待，以及 BeTree/ART/AERT
原流水线测试。各测试 300 秒超时；失败则不开始性能测量。此次默认不运行 sanitizer。
每次性能测量也检查 ticket 参考模型、GC 排空、更新计数和 worker 计数。
失败样本保存 stdout/stderr，不计为有效性能结果；正式重复未全部成功的配置不输出性能数字。
预热失败也会记录，最后返回非零退出码，应一起回传分析。

## 输出与回传

输出在独立 `build/server-experiment-日期-时间-PID/`，不覆盖历史数据：

- `metadata.json`：参数、机器、编译器、提交、源码/二进制哈希、编译/测试结果、完成状态。
- `*.test.log` / `*.build.log`：正确性与编译日志。
- `samples.jsonl`：每次命令、原始 stdout/stderr、状态和测量值，含预热。
- `summary.csv` / `summary.md`：正式测量中位数；valid/expected 必须为 3/3。
- 同级 `.tar.gz`：上述结果的打包，不包含二进制。失败时也生成。

终端最后打印 `RESULT_ARCHIVE=...`；回传该文件即可。
正常结束为 `COMPLETE; failed_samples=0`，但仍需分析性能，而非直接认定加速。
指标：ms 为完成所有更新的耗时，Mops/s 为百万操作/秒，peak MiB 为峰值进程内存，
live MiB 为最终可达树的请求字节，retired MiB 为累计退休字节，append % 为 fat 追加比例。
RSS 包含基准自身和参考模型，累计退休字节不是峰值内存。
先比较同 P/worker/负载/快照下 fat 开关，再比较同模式/槽数的不同 worker。
