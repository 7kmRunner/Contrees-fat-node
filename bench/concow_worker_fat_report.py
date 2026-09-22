"""Summarize paired cyclic entry/worker measurements without a scalability claim."""
import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(__doc__)
    parser.add_argument('inputs',nargs='+',type=Path)
    parser.add_argument('--output',type=Path,default=Path('bench/results/concow_worker_fat_summary.md'))
    args=parser.parse_args()
    grouped=defaultdict(list)
    total=0
    settings=None
    for path in args.inputs:
        metadata=json.loads(path.with_suffix('.metadata.json').read_text())
        if not metadata['completed']:raise ValueError(f'incomplete run: {path}')
        current=tuple(metadata['arguments'][k] for k in ('records','updates','runs','warmups'))
        if settings is not None and current!=settings:raise ValueError('incompatible workload or repeat settings')
        settings=current
        rows=list(csv.DictReader(path.open()))
        for r in rows:
            if r['reference_ok'].lower()!='true' or int(r['pending_bytes']):raise ValueError('invalid sample')
            key=tuple(int(r[k]) for k in ('pipeline_threads','pinned','slots','worker_fat','workers'))+(r['pattern'],)
            grouped[key].append(r)
        total+=len(rows)
    def median(key,metric):return statistics.median(float(r[metric]) for r in grouped[key])
    lines=['# 原 cyclic 架构的 worker fat-node 实验','',
           f'2026-09-17，Mac ARM64。共 {total} 次正式测量；每配置预热 {settings[3]} 次、正式 {settings[2]} 次，进程顺序运行，配置顺序随机。',
           '两种追加模式、slots=0 基线都使用本轮 monitor 独占 GC。4 个客户端，固定随机请求，每次校验按 ticket 排序的参考模型及 GC 排空。',
           'P 表示模板参数；实际 cyclic 线程数为 P+1，另有 monitor、worker 和客户端线程。', '',
           '## worker 模式的耗时与扩展性','',
           '表中时间为中位数毫秒；1→2/4 为同一 worker 模式的吞吐比，>1 表示变快。entry/worker 比比较相同槽数、4 workers，>1 表示 worker 模式更快。', '',
           '| P | 请求 | 固定旧快照 | slots | 1 worker ms | 2 workers ms | 4 workers ms | 1→2 | 1→4 | entry/worker（4） |',
           '|---|---|---|---|---|---|---|---|---|---|']
    for pipes in sorted({k[0] for k in grouped}):
        for pattern in ('uniform','hot'):
            for pinned in (0,1):
                for slots in (2,4,8):
                    keys=[(pipes,pinned,slots,1,w,pattern) for w in (1,2,4)]
                    times=[median(k,'elapsed_seconds') for k in keys]
                    entry=median((pipes,pinned,slots,0,4,pattern),'elapsed_seconds')
                    lines.append(f'| {pipes-1} | {pattern} | {pinned} | {slots} | '+
                        ' | '.join(f'{t*1000:.2f}' for t in times)+f' | {times[0]/times[1]:.2f} | {times[0]/times[2]:.2f} | {entry/times[2]:.2f} |')
    lines+=['','## 复制量与内存','',
            '以下固定 4 workers、worker slots=8，相对同 P/请求/快照下 slots=0。退休字节是累计分配后退休的请求字节，不能当作峰值 RSS；live 是最终可达树的请求字节。RSS 包含上下文环、线程、allocator 和参考模型。','',
            '| P | 请求 | 旧快照 | 退休节点减少 | 退休字节减少 | live / 基线 | 峰值 RSS / 基线 | worker8 / 基线吞吐 |',
            '|---|---|---|---|---|---|---|---|']
    for pipes in sorted({k[0] for k in grouped}):
        for pattern in ('uniform','hot'):
            for pinned in (0,1):
                base=(pipes,pinned,0,0,4,pattern);fat=(pipes,pinned,8,1,4,pattern)
                ratio=lambda field:median(fat,field)/median(base,field)
                lines.append(f'| {pipes-1} | {pattern} | {pinned} | {100*(1-ratio("retired_nodes")):.1f}% | {100*(1-ratio("retired_bytes")):.1f}% | {ratio("live_requested_bytes"):.2f} | {ratio("peak_rss_bytes"):.2f} | {1/ratio("elapsed_seconds"):.2f} |')
    lines+=['','## 解读边界','',
            'worker 数增加后的收益必须结合同槽数 entry 对照看：把每次追加交给 worker 会增加分派和通信，不能仅因 worker 模式的单 worker 较慢，就把相对加速当成优于已有方案。热点仍受同路径依赖与连续提交前缀约束。',
            f'只有 {settings[2]} 次重复，没有 CPU 绑核或频率控制。原始 CSV 保留每次测量，不能将接近 1 的比值视为稳定收益；也不据此宣称线性或普遍扩展。',
            '默认保留 entry 模式，worker 模式用 --fat-workers 显式开启。受控测试证明重叠与协议正确性，性能实验单独报告。','',
            '## 原始数据与复现','']
    for path in args.inputs:
        rows=list(csv.DictReader(path.open()))
        lines += [f'- [{path.name}]({path.name})：records={rows[0]["records"]}，updates={rows[0]["updates"]}；[命令、源文件和二进制哈希]({path.with_suffix(".metadata.json").name})。']
    lines+=['','协议：[worker fat-node](../../docs/CONCOW_WORKER_FAT.md)。历史入口准入实验没有被本报告覆盖。']
    args.output.write_text('\n'.join(lines)+'\n')

if __name__=='__main__':main()
