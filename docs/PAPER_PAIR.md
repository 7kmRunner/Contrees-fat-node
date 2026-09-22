# 论文负载：原版与 fat-node 的两版本对照

依据用户提供的 `3802030.pdf`，论文 *Concurrent Path-Copying Update to Tree Structures*，
第 6 节、印刷页 153:20–23。论文引用 [2] 指向 CUHK-DBGroup/Contrees。
原版固定为本项目保留的上游提交 `e487e91d31552685ea4d459223fc78375ed66591`。
正文没有给出提交哈希，因此称为“论文链接仓库的固定原始版本”，不声称已证明它就是作者当年测量的精确提交。

## 对齐与边界

| 项目 | 论文 | 本脚本正式模式 |
|---|---|---|
| 初始记录 | 100,000,000 | 相同 |
| 每轮操作 | 1,000,000 | 相同；原仓库 spec 写的是 10,000,000，本脚本仅在独立生成器副本中按正文覆盖 |
| 客户端 | 32 | 相同 |
| 重复 | 10 次独立运行，平均值 | 每轮重新启动原版 YCSB 导出器，两个版本共用该轮相同数据，取每轮吞吐算术平均 |
| 默认负载 | 图 9：插入 | 原版 workload_update.spec，insertproportion=1 |
| 编译 | GCC、C++20、全优化、mimalloc | g++、C++20、O3、原 Makefile 的本机 SIMD/AES 标志、mimalloc |
| CPU | 双路 Xeon Gold 5320，26 核/路 | 用户服务器同型号；记录实际 lscpu |
| 内存 | 128 GB | 用户服务器约 1 TiB，不强行限制，明确记录差异 |
| GC | 上游代码无回收支持 | 原版不改，fat 版关闭 GC；不把 GC 的收益混入比较 |
| fat 模式 | 无 | 当前原流水线，入口追加，默认 8 槽；不使用独立带锁批次控制器 |

默认只比较两种并发实现，不是整篇论文所有图和所有对手系统的复现。
没有 Contreap、P-Tree、TPC-H，也没有添加串行/inplace 对照，因此不能从本表计算论文中的“相对串行加速”。
原版 `src/main.cpp`、调度器和树文件不修改；fat 版也从执行时的 Git HEAD 导出，避免混入未提交改动。
双方使用各自的原 CLI 计时入口。数据生成不计入 process time，但进程峰值 RSS 包含输入数组、建树和 allocator，
**不能把 RSS 称为图 13 的纯索引空间开销或完整内存实验复现**。
未控制 NUMA、绑核、频率；记录 affinity/机器/开始时负载。机器有相同 CPU 不代表所有实验条件相同。

## 三个实验入口

先在 Linux x86-64 服务器安装 g++、Python 3、mimalloc 开发库、GNU time。
Ubuntu/Debian：`sudo apt-get install build-essential python3 libmimalloc-dev time`。

```bash
# 先用小规模确认两个原生二进制、原版生成器和日志流程可用。
python3 -u bench/paper_pair.py --smoke

# 第一轮正式实验：1 亿记录、100 万插入、10 次独立运行、四棵树。
python3 -u bench/paper_pair.py --suite insert

# 可选：论文图 10/11 的两种混合负载，每种 5 个比例。
python3 -u bench/paper_pair.py --suite mixed

# 可选：图 9 的 2/3/4/5/6 pipes × 2/4/8/16/32 workers。
# 仅原版和 fat 两个并发实现，未包含图中的串行虚线。
python3 -u bench/paper_pair.py --suite scaling
```

默认/混合负载采用论文为混合实验选择的配置：
B+Tree/ART：2 pipes、8 workers（CLI `-p 1 -w 8`）；
BeTree/AERT：4 pipes、16 workers（CLI `-p 3 -w 16`）。
这里 `-p` 不包含出口 pipe，论文 pipe 数等于 CLI P+1。
默认正式实验最多 80 次性能进程；mixed 最多 800 次；scaling 最多 2000 次。
每次都会重建 1 亿记录，明显慢于先前的小规模矩阵，不承诺完成时长。
可用 `--trees btree` 只跑指定树，`--slots 2/4/8` 指定 fat 槽数；这些选择保存在元数据中。
`--timeout` 默认每次基准进程 1800 秒（包含建树），数据生成上限 3600 秒。
原版需要 AVX512BW/VL 的 ART/AERT 指令，目标 Gold 5320 支持；不要把脚本用于不支持的 CPU。

混合负载直接使用原仓库的 lookup10/30/50/70/90、scan10/20/30/40/50 spec。
扫描比例是查询操作内部的比例：例如 scan50 是总操作中 50% 插入、25% 点查、25% 扫描；
扫描长度依照原 spec（最大 1000，均匀分布）。
ART/AERT 当前 fat 槽只优化已有键更新，论文的纯插入负载可能没有追加收益；不更换成覆盖更新来制造优势。

## 正确性和原版计时限制

每个配置先用原版 YCSB 产生 10 万记录/10 万操作，运行独立的 `paper_preflight.cpp`：
保留原算法文件不变，32 客户端执行，等待最大实际 ticket，检查初始键和全部写入键的最终值。
原导出器所有值均等于键，所以无须推断并发写入顺序。它不验证并发查询/扫描返回值，不代替全规模的正确性证明。
任何一侧失败，就跳过该配置的两侧正式测量；失败输出保存在 metadata 中，不把错误结果当作快。

原版主程序按写入条数等待，BeTree checkpoint 可能消耗额外 ticket；当前主程序已修复为等待最大实际 ticket。
检查器会记录 `last_ticket`、`writes` 和 `ticket_gap`。原版 BeTree 一律标记 `timing_warning`（即使小规模预检未触发 checkpoint），其他树检测到 ticket gap 也会标记；
保留原 CLI 原始计时，但自动汇总**不计算加速比**。不偷偷修复基线，也不把可疑计时称为准确吞吐。
检查未出现 gap 也不证明全规模不会发生，BeTree 原版计时依然需要关注。

## 结果

每次独立生成 `build/paper-pair-日期-时间-PID/`：

- `metadata.json`：版本、全部归档源文件 SHA256、机器/编译器、构建命令、预检、数据 SHA256。
- `specs/`：按正文调整规模后的每次负载配置。
- `samples.jsonl`：每次命令、stdout/stderr、处理时间、GNU time 峰值 RSS 和有效性标记。
- `summary.csv` / `summary.md`：均值、标准差、通过次数；仅符合条件的两版本配置计算加速比。
- `logs/`：生成、编译、运行和 GNU time 原始输出。
- 同级 `.tar.gz`：以上结果文件，不含源码归档、二进制或大数据集。

每轮两版本共用同一数据文件，记录 SHA256；完成该轮后删去生成的数据以节省磁盘。
脚本不会删除现有数据或覆盖先前结果。发生普通错误或超时仍打包；强制杀进程或机器断电不保证最终打包完成。
最后把 `RESULT_ARCHIVE=` 对应文件回传即可。`--smoke` 的数据不能用作论文规模结论。

## 准备阶段验证（2026-09-22）

- 原版校验器以 x86-64/AVX512 目标编译通过；没有在 Mac 上执行这些 AVX512 指令。
- 当前校验器在 Mac ARM64 上通过四树 × 插入/点查混合/扫描混合共 12 个小规模检查
  （1000 初始记录、2000 操作、4 客户端、1 worker）。
- B+Tree 的 10 万/10 万、32 客户端、8 worker 预检通过；ART 同规模、32 客户端、8 worker
  在本机 120 秒超时。不能把它归因为某个确定原因或宣称服务器一定正常；服务器预检会重新检查并记录。
- 汇总均值、失败样本排除、计时警告排除和子进程超时终止已检查。
- Linux 上的 GCC/mimalloc 原生两版本完整流程尚需服务器 `--smoke` 验证。
