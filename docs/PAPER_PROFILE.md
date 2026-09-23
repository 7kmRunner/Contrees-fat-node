# 论文规模更新阶段的 CPU 采样

上轮正式三组对照已经是论文规模：1 亿初始记录、100 万插入、32 客户端、8 workers、10 次重复。
“小规模校验”只指计时前的最终值预检；正式计时没有全规模逐键校验。
本轮用于定位吞吐退化，仍使用同规模和线程参数，但默认每组 3 次诊断采样，不是正式性能重复。

## 运行

在运行过三组实验的 Linux x86-64 服务器上：

```bash
# Ubuntu 按运行中的内核安装 perf；如缺包，请把安装错误发回。
sudo apt-get install -y linux-tools-common "linux-tools-$(uname -r)"
python3 -u bench/paper_profile.py
```

脚本自动选择最近的、成功完成的 `--include-worker` 三组结果目录，校验里面原版和 fat 的
完整归档源文件哈希；只编译额外的诊断二进制，不修改归档源码。
可以用 `--source-run build/paper-pair-20260923-105002-2466379` 指定来源。
不要删除旧结果目录中的 original-source 和 fat-source。
原版/入口追加/worker 追加每组默认三次；每轮运行顺序随机。
`--smoke` 缩为 10 万/10 万，只用于检查诊断环境。默认命令仍为 1 亿/100 万。

先检查 `perf record -e cpu-clock:u` 权限；无权限或 perf 缺失时，在生成大数据之前停止并打包日志。
脚本不改变内核安全配置，不自动 sudo 运行基准。若权限检查失败，把日志/结果包回传，再按服务器管理要求处理。

## 测量边界

使用 Linux perf 5.15 文档支持的 `--delay=-1 --control=fifo:ctl,ack`：
https://github.com/torvalds/linux/blob/v5.15/tools/perf/Documentation/perf-record.txt

诊断包装器将原 CLI 的 timer 类型替换为一个外层计时器。原 CLI 第一次 start/count 包围建树，
第二次包围提交/执行/等待提交。仅第二次 start 发送 enable 并等 ack；第二次 count 发送 disable 并等 ack。
算法、调度器、树文件保持原样；入口依赖、线程数、GC 设置不变。
附加 `-g -fno-omit-frame-pointer` 保留调用栈，因此诊断二进制和未插桩二进制不完全相同。
用每线程继承的 `cpu-clock:u` 事件，199 Hz，采用户态 CPU 样本和调用栈；不采建树阶段。
CPU 自旋等待会占样本；睡眠/被调度出去的时间不会等同地反映为样本。
百分比是 CPU 样本分布，不能直接称为某阶段墙钟占比，不能只凭函数占比证明因果。
流水线函数可能内联，因此同时输出调用路径、源码行和原始栈帮助定位。

原版未修复，仅增加诊断计时开关；不把它称为未经插桩的正式原版性能。
从原上游生成器重新生成相同插入负载，记录数据哈希；全部三组使用同一数据文件。
脚本没有新增全规模结果校验、GC 或算法优化。`--runs` 可以调整诊断重复数。

## 结果

`build/paper-profile-日期-时间-PID/` 保存：

- metadata.json：源实验元数据、源码/二进制/数据哈希、命令和状态。
- logs：编译、权限探测、基准 stdout/stderr，包含 enable/disable 确认。
- profiles/*-self.txt：函数自身 CPU 样本分布。
- profiles/*-callers.txt：包含下级调用的路径分布。
- profiles/*-lines.txt：函数和源码行分布。
- profiles/*-stacks.txt 与 *.data：解码调用栈与原始 perf 数据。

最终 `.tar.gz` 包含报告、原始采样和日志，不含 1 亿记录数据集或二进制。
回传 `RESULT_ARCHIVE=` 文件。未运行成功时也会尽可能打包；不会把空采样标为成功。
Mac 上只能验证包装器编译与 FIFO 开关协议；Linux perf 的完整运行需要在服务器确认。

## perf 5.15 ACK 兼容修复

服务器第一次采样在开启事件后因 `unexpected perf acknowledgement` 退出，没有有效诊断结果。
perf 5.15 的 evlist__ctlfd_ack 使用 sizeof(ACK_TAG) 写出确认消息，含尾部 NUL；
旧包装器严格比较 `ack\n`，因此拒绝正常回复。现过滤 NUL 并保留对非 ack 消息的拒绝。
已测试普通回复、带 NUL、分片回复、延迟到下一次交换的 NUL，以及错误回复拒绝。
脚本现在在编译树和生成 1 亿数据前先执行真实 perf 的 250ms 握手测试。
内核符号权限警告不是本次退出原因；诊断只采用户态，不要求为此修改 kptr_restrict 或改用 root。
参考实现：https://github.com/torvalds/linux/blob/v5.15/tools/perf/util/evlist.c
