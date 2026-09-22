"""Render medians from the matched comparison; fail on incomplete data."""
import csv
from collections import defaultdict
from pathlib import Path
from statistics import median
ROOT=Path(__file__).resolve().parents[1]
rows=list(csv.DictReader((ROOT/'bench/results/multitree_original_current_comparison.csv').open()))
assert len(rows)==144
assert all(int(r['mismatches'])==0 for r in rows if r['mode']=='latest')
groups=defaultdict(list)
for r in rows:groups[r['tree'],r['workload'],int(r['workers']),r['mode']].append(r)
assert len(groups)==48 and all(len(v)==3 and {int(r['run']) for r in v}=={1,2,3} for v in groups.values())
def stat(t,l,w,m):
    rs=groups[t,l,w,m]
    return median(float(r['seconds'])*1000 for r in rs),median(int(r['peak_rss_bytes'])/2**20 for r in rs)
def valid(t,l,w,m):
    return all(int(r['mismatches'])==0 for r in groups[t,l,w,m])
names={'btree':'B+Tree','art':'ART','aert':'AERT','betree':'BeTree'}
text=['# 四棵树与原 ConCow 架构的统一对比\n',
'''本轮重新运行四棵树的全部配置，不拼接历史结果。原基线是提交
`e487e91d31552685ea4d459223fc78375ed66591` 的隔离导出，按原 main.cpp 路由：
B+Tree 使用 `concow_cyclic<interface,3>`，其它三树使用 `concow<interface>(3,workers,root)`。
原基线应用 ARM64 acquire/release 和 MPSC 同步修复、B+Tree/BeTree 完整叶复制修复、
ART/AERT Node16 的 ARM64 标量兼容路径，以及零长度前缀避免移位 64 位的修复。
两边在 ARM 上均使用相同 Node16 标量查找。此基线称为“原架构加兼容性与正确性修复”，
不能称为未修改原版或论文硬件复现。新架构为当前 `concow_fat`，2 槽、批量物化开启，
队列容量 4096、批次上限 256、并行门槛 16 叶组。两边 GC 关闭。

## 实验方法

- Mac ARM64、Apple clang 17、mimalloc，C++20/O3/NDEBUG/pthread；二进制和源码哈希见 metadata JSON。
- 每个独立进程先建树：100,000 个键（0..99,999），初始值 key+1000；BeTree 初始 boundary 参数 2。
- 每轮 300,000 次写操作，4 个提交客户端，每次从共享游标领取 64 条请求。
- 均匀更新：随机种子 182，mt19937_64 生成值对 100,000 取模，选已有键；每次值为请求序号+1。结束仍为 100,000 键。
- 插入：新键 100,000..399,999，每键插入一次，随机打乱顺序；结束为 400,000 键。
- 相同输入请求集合；并发实际提交顺序可能不同，按各次返回 ticket 的最大值建立最终参考结果。
- 计时从创建提交客户端前开始，包含客户端创建、提交、更新和等待全部提交完成；不含输入生成、建树、最终校验和析构。
- 完成后逐键检查全部预期键的值，包括未更新基础键；本驱动不测并发读、长快照、删除和范围查询性能。
- 峰值 RSS 用 getrusage，包含建树、输入、参考模型、调度器、所有保留历史及分配器，不等于净树大小；读取发生在最终校验后。
- 每配置一次预热、三次正式独立进程运行，配置顺序交替；实验进程串行执行，报告三次中位数。
- 4 树 × 2 负载 × 3 worker 参数 × 2 架构 × 3 正式重复 = 144 条；逐次校验状态见 CSV，失败配置不计算加速比。

## 完整结果

加速比 = 原耗时 / 新耗时；大于 1 表示新版更快。RSS 降幅 = 1 - 新 RSS / 原 RSS。
''',
'| 树 | 负载 | workers | 原 ms | 新 ms | 加速比 | 原 RSS MiB | 新 RSS MiB | RSS 降幅 |',
'|---|---|---:|---:|---:|---:|---:|---:|---:|']
for t in names:
    for l in ('update','insert'):
        for w in (1,2,4):
            a,b=stat(t,l,w,'baseline'),stat(t,l,w,'latest')
            if valid(t,l,w,'baseline'):
                text.append(f'| {names[t]} | {l} | {w} | {a[0]:.2f} | {b[0]:.2f} | {a[0]/b[0]:.2f}× | {a[1]:.2f} | {b[1]:.2f} | {(1-b[1]/a[1])*100:.1f}% |')
            else:
                text.append(f'| {names[t]} | {l} | {w} | 校验失败 | {b[0]:.2f} | — | {a[1]:.2f}（失败运行） | {b[1]:.2f} | — |')
text+=['\n## 各取已测最快配置\n','| 树 | 负载 | 原 workers | 新 workers | 原 ms | 新 ms | 加速比 | RSS 降幅 |','|---|---|---:|---:|---:|---:|---:|---:|']
for t in names:
    for l in ('update','insert'):
        candidates=[w for w in (1,2,4) if valid(t,l,w,'baseline')]
        if not candidates:
            text.append(f'| {names[t]} | {l} | — | — | 校验失败 | — | 不可比较 | — |');continue
        wa=min(candidates,key=lambda w:stat(t,l,w,'baseline')[0]);wb=min((1,2,4),key=lambda w:stat(t,l,w,'latest')[0])
        a,b=stat(t,l,wa,'baseline'),stat(t,l,wb,'latest')
        text.append(f'| {names[t]} | {l} | {wa} | {wb} | {a[0]:.2f} | {b[0]:.2f} | {a[0]/b[0]:.2f}× | {(1-b[1]/a[1])*100:.1f}% |')
text += ['\n## 正确性记录\n', f'正式运行 {len(rows)} 次，零差异 {sum(int(r["mismatches"])==0 for r in rows)} 次。']
for key,rs in groups.items():
    if any(int(r['mismatches']) for r in rs):
        text.append(f'- {key}: 三次差异数 {[int(r["mismatches"]) for r in rs]}。')
text += ['小规模（10,000 初始键、10,000 插入）AERT 原并行控制器预检出现 1 个键差异；同输入原 SeqCow 的诊断运行通过。正式规模（100,000 初始键、300,000 操作）本轮全部通过。尚未定位小规模问题根因，不将失败耗时作为有效性能。预检和测量日志见 `multitree_original_current_preflight.log`。']
text+=['''
## 解释范围

相同 worker 数不等于相同总线程预算：原架构为 4 个流水线线程、1 个 monitor 和 w 个 worker，
新版为 1 个协调线程和 w 个池线程，另有两边相同的 4 个客户端。新版 w=1 时协调线程直接
处理叶组，池线程空闲。原架构默认 65,536 上下文，新版队列 4,096。
因此本表衡量完整架构差异，包含 fat-node、批量路径合并、调度和内存布局的共同效果；
不能将 RSS 降幅全部归为 fat-node，不能将速度收益归为多 worker 扩展性。
关闭 GC 时历史内存累积，本结果不能预测开启 GC 后的降幅，也不能外推到热点、读写混合、
论文规模或原 x86 SIMD 平台。三次短测的细小差异需更多重复确认。

## 复现

在仓库根目录执行 `python3 bench/multitree_original_compare.py`（脚本内为本机 mimalloc 路径），
随后执行 `python3 bench/multitree_original_report.py`。
前者重新导出基线、应用已记录补丁、编译八个二进制、运行小规模校验及完整矩阵。
原始数据：`multitree_original_current_comparison.csv`；环境与哈希：同名 `.metadata.json`。
''']
(ROOT/'bench/results/multitree_original_current_summary.md').write_text('\n'.join(text)+'\n')
print('Report written: bench/results/multitree_original_current_summary.md')
