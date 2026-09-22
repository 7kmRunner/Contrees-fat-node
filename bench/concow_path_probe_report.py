"""Render the fixed native path-probe experiment from its retained raw CSVs."""
import csv
import json
from pathlib import Path
import statistics


def load(name):
    path = Path('bench/results') / name
    meta = json.loads(path.with_suffix('.metadata.json').read_text())
    if not meta['completed']:
        raise RuntimeError(f'incomplete matrix: {path}')
    rows = list(csv.DictReader(path.open()))
    groups = {}
    for row in rows:
        key = (row['pattern'], int(row['pinned']), int(row['workers']),
               int(row['slots']), int(row['local_probe']))
        groups.setdefault(key, []).append(row)
    if len(groups) != 84 or any(len(rs) != meta['arguments']['runs'] for rs in groups.values()):
        raise RuntimeError(f'incomplete groups: {path}')
    return rows, groups, meta


def median(rows, field):
    return statistics.median(float(row[field]) for row in rows)


def render():
    rows, groups, meta = load('concow_path_probe_arm64.csv')
    p1rows, p1groups, p1meta = load('concow_path_probe_p1_arm64.csv')
    if meta['arguments']['records'] != p1meta['arguments']['records'] or meta['arguments']['updates'] != p1meta['arguments']['updates']:
        raise RuntimeError('incomparable matrix sizes')
    updates = meta['arguments']['updates']
    baseline = groups['uniform', 0, 2, 0, 1]
    optimized = groups['uniform', 0, 2, 4, 1]
    old_gate = groups['uniform', 0, 2, 4, 0]
    traffic_drop = 100*(1-median(optimized, 'retired_bytes')/median(baseline, 'retired_bytes'))
    time_drop = 100*(1-median(optimized, 'elapsed_seconds')/median(baseline, 'elapsed_seconds'))
    text = [
        '# B+Tree cyclic 路径级准入实验', '', '2026-09-16。', '',
        '本轮已把全局结构前沿准入细化为目标路径的构造就绪检查，并保留原阶段 COW。',
        '**严格 lock-free 尚未实现**；暂停提交者的反例及论文核对见',
        '[进展审查](../../docs/CONCOW_PROGRESS_AUDIT.md)。本报告将减少复制、内存占用、',
        '执行重叠与 worker 吞吐扩展分别报告，不把其中一项当成其它项的证明。', '',
        f'四级流水线、均匀更新、2 workers、无长快照：4 槽新准入的追加率为 '
        f'{100*median(optimized,"appended_updates")/updates:.2f}%，旧全局门为 '
        f'{100*median(old_gate,"appended_updates")/updates:.2f}%。相对同架构 0 槽，'
        f'累计退休字节减少 {traffic_drop:.1f}%，耗时中位数减少 {time_drop:.1f}%。', '',
        '## 配置与可比性', '',
        f'- {meta["platform"]}，系统报告 {meta["cpu_count"]} 个逻辑 CPU；Apple clang 17、mimalloc、`-O3 -DNDEBUG`。',
        f'- {meta["arguments"]["records"]:,} 条初始记录，{updates:,} 次已有 key 更新，4 个提交客户端，GC 开启。',
        '- P=3 为 4 个 pipeline 线程，P=1 为 2 个；另有 1/2/4 个 worker 和 1 个 monitor。',
        '- uniform 均匀随机；hot 为 90% 更新落在前 100 个 key。pinned 保留初始快照至更新完成。',
        f'- 每配置 {meta["arguments"]["warmups"]} 次预热、{meta["arguments"]["runs"]} 次正式测量；配置顺序用固定 seed 打乱，各进程顺序运行。',
        f'- 两种深度共 {len(rows)+len(p1rows)} 条正式测量，全部全 key 参考模型校验正确，GC pending=0，退休字节全部回收。',
        '- slots=0 与 slots=2/4/8 使用同一深度的同一二进制。global/local 也是运行时选项。',
        '- 已包含现有原子同步修复、copy_leaf 修复及本轮就绪标记，不冒充未修改的原论文代码。',
        '- 各样本仅数百毫秒；未绑定 CPU，未做置信区间分析，不把小差异当成稳定扩展收益。', '',
        '## 全局门与路径门：P=3，uniform，2 workers，无长快照', '',
        '| 槽数 / 准入 | 耗时中位数 [min,max] ms | 追加率 | 累计退休 MiB | 峰值 RSS MiB | 最终树 live MiB |',
        '|---|---:|---:|---:|---:|---:|',
    ]
    for slots, local in ((0,1),(2,0),(2,1),(4,0),(4,1),(8,0),(8,1)):
        rs = groups['uniform',0,2,slots,local]
        times = [float(r['elapsed_seconds'])*1000 for r in rs]
        label = '关闭 fat' if slots == 0 else ('路径' if local else '全局')
        text.append(f'| {slots} / {label} | {statistics.median(times):.2f} [{min(times):.2f},{max(times):.2f}] | '
                    f'{100*median(rs,"appended_updates")/updates:.2f}% | {median(rs,"retired_bytes")/2**20:.2f} | '
                    f'{median(rs,"peak_rss_bytes")/2**20:.2f} | {median(rs,"live_requested_bytes")/2**20:.2f} |')
    text += ['', '累计退休字节衡量被替换节点及 sidecar 的累计释放流量，不是峰值或全部分配量。',
             'RSS 是进程峰值，包含上下文环、线程、请求与参考模型。最终 reachable live bytes',
             '在启用 fat 后会上升，因为保留 sidecar；不能据退休流量减少声称所有内存指标都下降。', '',
             '## 保留旧快照：P=3，uniform，2 workers，路径门', '',
             '| 槽数 | 耗时中位数 ms | 峰值 RSS MiB | 最终树 live MiB |',
             '|---|---:|---:|---:|']
    for slots in (0,2,4,8):
        rs = groups['uniform',1,2,slots,1]
        text.append(f'| {slots} | {1000*median(rs,"elapsed_seconds"):.2f} | {median(rs,"peak_rss_bytes")/2**20:.2f} | {median(rs,"live_requested_bytes")/2**20:.2f} |')
    text += ['', '## Worker 扩展性：无长快照，路径门', '',
             '吞吐 = 更新数 / 耗时，以下为各配置中位数，单位 M updates/s。', '',
             '| pipeline 线程 | 负载 | 槽数 | 1 worker | 2 workers | 4 workers |',
             '|---|---|---:|---:|---:|---:|']
    for pipes, gs in ((4,groups),(2,p1groups)):
        for pattern in ('uniform','hot'):
            for slots in (0,2,4,8):
                values = [updates/median(gs[pattern,0,w,slots,1],'elapsed_seconds')/1e6 for w in (1,2,4)]
                text.append(f'| {pipes} | {pattern} | {slots} | ' + ' | '.join(f'{v:.3f}' for v in values) + ' |')
    text += ['', '两个受控深层 handler 必须在任一被释放前都进入，测试证明结构 worker 存在真实执行重叠。',
             '这与表中的整体吞吐扩展是不同证据。追加仍在原有序入口执行；没有实现多 worker 同叶追加。', '',
             '**扩展性验收仍未通过**：本轮多数 fat 配置在增加 worker 后变慢；较浅流水线也未形成',
             '1→2→4 workers 持续提升。个别热点配置 2 workers 高于 1 worker，但 4 workers 回落，',
             '且追加率随调度交错变化，不能据单个中位数差异认定已解决并行扩展问题。', '',
             '## 热点限制：P=3，hot，2 workers，无长快照', '',
             '| 槽数 / 准入 | 耗时中位数 ms | 追加率 | 累计退休 MiB |',
             '|---|---:|---:|---:|']
    for slots, local in ((0,1),(4,0),(4,1),(8,1)):
        rs = groups['hot',0,2,slots,local]
        text.append(f'| {slots} / {"路径" if local else "全局"} | {1000*median(rs,"elapsed_seconds"):.2f} | '
                    f'{100*median(rs,"appended_updates")/updates:.2f}% | {median(rs,"retired_bytes")/2**20:.2f} |')
    text += ['', '热点更多地遇到尚未完成的同一路径，回退原 COW。该结果不能用均匀更新的高追加率替代。', '',
             '## 正确性与复现', '',
             '- 36 组 cyclic 并发参考模型、8 组满树级联分裂、旧快照、同叶冲突、跨环复用及 GC 排空通过。',
             '- 受控暂停的同路径拒绝、不同路径追加、两个结构 worker 重叠测试通过。',
             '- SeqCow B+Tree 与独立批次 B+Tree 回归通过；系统分配器 UBSan 通过。ASan/TSan 未通过验收。',
             '- 提交空洞测试成功复现阻挡，不计为无锁通过；完整记录见 [验证清单](concow_path_probe_validation.json)。', '',
             '```sh',
             'make test-concow-cyclic-fat test-concow-progress bench-concow-cyclic-fat bench-concow-cyclic-fat-p1 CC=clang++',
             'python3 bench/concow_path_probe_matrix.py',
             'python3 bench/concow_path_probe_matrix.py --binary bench/concow_cyclic_fat_p1_bench --output bench/results/concow_path_probe_p1_arm64.csv',
             'python3 bench/concow_path_probe_report.py',
             '```', '',
             '原始数据：[四级流水线](concow_path_probe_arm64.csv)、[两级流水线](concow_path_probe_p1_arm64.csv)。',
             '各 CSV 对应 `.metadata.json` 保存参数、完整执行顺序、编译器、源码和二进制 SHA-256。', '']
    Path('bench/results/concow_path_probe_summary.md').write_text('\n'.join(text))


if __name__ == '__main__':
    render()
