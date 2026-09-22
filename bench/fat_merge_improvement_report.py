import csv
import json
import math
import statistics as st
from collections import defaultdict
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'bench/results/fat_merge_improvement_20260918'
meta=json.loads(BASE.with_suffix('.metadata.json').read_text());assert meta['completed']
def load(kind,fields):
    grouped=defaultdict(dict)
    for row in csv.DictReader(open(str(BASE)+'_'+kind+'.csv')):
        assert row['reference_ok'].lower()=='true'
        grouped[tuple(row[f] for f in fields)][(row['run'],row['version'])]=row
    for group in grouped.values():assert len(group)==10
    return grouped
micro=load('micro',['worker','inserts','slots'])
cyclic=load('cyclic',['pipeline_threads','workers','pattern','worker_fat','slots'])
def stats(group,field):
    before=[float(group[(str(r),'before')][field]) for r in range(1,6)]
    after=[float(group[(str(r),'after')][field]) for r in range(1,6)]
    ratio=[a/b for a,b in zip(before,after)]
    return st.median(before),st.median(after),st.median(ratio),min(ratio),max(ratio)
lines=['# B+Tree fat-node 物化顺序合并优化','',
       '2026-09-18。只修改 lib/trees/btree/update_cow.hpp；保留原 cyclic 调度、槽位预留/发布、提交与 GC 协议。节点、slot、context 的大小不变。','',
       '基础键本来有序：改为只排序最多 9 条增量并与 base 合并；worker 填基础值时顺序扫描 source，避免逐 key 从头查找。',
       '不声称减少路径复制次数或节点内存，本轮目标是减少每次物化的 CPU 工作。','',
       '## 物化局部实验','',
       '满叶 15 条基础记录，2/4/8 槽；update 反复修改同一 key，insert 使用新 key 并触发分裂。每进程 50 万次物化，包含分配/释放，建源叶和参考模型校验不计时。固定源叶重复使用，属于局部热缓存实验。',
       '5 次正式、1 次预热，before/after 顺序随机、进程顺序执行；加速比为每轮 before/after 的中位数。','',
       '| 模式 | 负载 | slots | before ns/次 | after ns/次 | 加速比 |','|---|---|---|---|---|---|']
for (worker,inserts,slots),group in sorted(micro.items()):
    for r in range(1,6):assert group[(str(r),'before')]['checksum']==group[(str(r),'after')]['checksum']
    a,b,ratio,_,_=stats(group,'seconds')
    lines.append(f'| {"worker" if worker=="1" else "entry"} | {"insert" if inserts=="1" else "update"} | {slots} | {a*2000:.2f} | {b*2000:.2f} | {ratio:.2f}x |')
lines+=['','## 端到端实验','',
        'ARM64 原生、mimalloc、GC 开启、4 客户端、无固定旧快照；10 万初始记录、30 万次更新。P=1/3，workers=1/2，uniform/hot，slots=0/2/4/8；每配置预热一次、正式五次。共 560 次正式测量。',
        '相同输入种子，before/after 成对随机执行。所有运行按 ticket 校验结果并检查 GC 排空。下表速度比 >1 表示改进后更快；区间是五对速度比的最小–最大值，不是置信区间。','',
        '| P | workers | 请求 | 模式 | slots | before ms | after ms | 速度比 | 五对范围 |',
        '|---|---|---|---|---|---|---|---|---|']
summary=defaultdict(list)
for (pipes,workers,pattern,worker,slots),group in sorted(cyclic.items()):
    a,b,ratio,lo,hi=stats(group,'elapsed_seconds')
    mode='baseline' if slots=='0' else ('worker' if worker=='1' else 'entry')
    summary[mode].append(ratio)
    lines.append(f'| {int(pipes)-1} | {workers} | {pattern} | {mode} | {slots} | {a*1000:.2f} | {b*1000:.2f} | {ratio:.2f}x | {lo:.2f}–{hi:.2f} |')
lines+=['','## 汇总与限制','']
for mode,ratios in sorted(summary.items()):
    gm=math.exp(sum(math.log(r) for r in ratios)/len(ratios))
    lines.append(f'- {mode}：{len(ratios)} 个配置的成对中位速度比几何平均 {gm:.3f}x；配置间范围 {min(ratios):.3f}–{max(ratios):.3f}x。')
lines+=['',
        'slots=0 未经过本轮 fat 物化修改，其变化反映环境/调度波动。局部微基准收益不能直接等同于端到端吞吐收益；热点下的动态回退次数也可能随调度改变。五次重复未做显著性检验，不宣称所有负载稳定变快或 worker 扩展性已经解决。',
        '受控并发测试、两模式 72 组参考模型、16 组级联分裂、16 组 GC 参数、SeqCow 与独立批次控制器回归通过；受控测试和 cyclic 参考模型 UBSan 通过。','',
        '## 复现与数据','',
        '- [完整补丁](../btree_fat_linear_merge.patch)：before 头可从 after 在独立目录逆向应用该补丁得到。',
        '- [局部数据](fat_merge_improvement_20260918_micro.csv) · [端到端数据](fat_merge_improvement_20260918_cyclic.csv) · [二进制/源头哈希与逐次命令](fat_merge_improvement_20260918.metadata.json)。',
        '- 驱动：bench/btree_materialize_bench.cpp；矩阵：bench/fat_merge_improvement.py；报告：bench/fat_merge_improvement_report.py。',
        '- 矩阵读取 build/fat-merge-improvement 下 before-p1/p3、after-p1/p3、micro-before/after。cyclic 编译方式同 Makefile；micro 使用相同 C++20、O3、DNDEBUG 和 mimalloc 参数，before 的 include 路径指向保存的旧 node/context/update_cow 目录。']
BASE.with_suffix('.md').write_text('\n'.join(lines)+'\n')
