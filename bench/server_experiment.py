"""Four-tree correctness gate, then same-scheduler B+Tree measurements (stdlib only)."""
import argparse
import csv
import datetime
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import shlex
import statistics
import subprocess
import tarfile
import time
from collections import defaultdict


def execute(command, timeout):
    started = time.monotonic()
    try:
        p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return dict(command=command, status='ok' if p.returncode == 0 else 'process_failure',
                    returncode=p.returncode, stdout=p.stdout, stderr=p.stderr,
                    wall_seconds=time.monotonic()-started)
    except subprocess.TimeoutExpired as e:
        decode = lambda s: s.decode(errors='replace') if isinstance(s, bytes) else (s or '')
        return dict(command=command, status='timeout', stdout=decode(e.stdout),
                    stderr=decode(e.stderr), wall_seconds=time.monotonic()-started)
    except OSError as e:
        return dict(command=command, status='launch_failure', stdout='', stderr=str(e))


def summarize(out, rows, runs):
    groups = defaultdict(list)
    for row in rows:
        if row['run'] > 0:
            groups[tuple(row[k] for k in ('p', 'workers', 'pattern', 'pinned', 'mode', 'slots'))].append(row)
    summaries = []
    for key, samples in sorted(groups.items()):
        good = [r['measurement'] for r in samples if r['status'] == 'ok']
        s = dict(zip(('p', 'workers', 'pattern', 'pinned', 'mode', 'slots'), key))
        s.update(valid=len(good), expected=runs)
        # Incomplete configurations never receive a performance number.
        if len(good) == runs and len(samples) == runs:
            median = lambda f: statistics.median(r[f] for r in good)
            s.update(ms=median('elapsed_seconds')*1000,
                     mops=median('updates')/median('elapsed_seconds')/1e6,
                     peak_mib=median('peak_rss_bytes')/2**20,
                     live_mib=median('live_requested_bytes')/2**20,
                     retired_mib=median('retired_bytes')/2**20,
                     append_percent=100*median('appended_updates')/median('updates'))
        summaries.append(s)
    fields = ['p','workers','pattern','pinned','mode','slots','valid','expected',
              'ms','mops','peak_mib','live_mib','retired_mib','append_percent']
    with (out/'summary.csv').open('w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fields); writer.writeheader(); writer.writerows(summaries)
    lines = ['# Server experiment', '',
             'Current B+Tree cyclic scheduler; GC enabled in every sample. This is not the untouched paper implementation.',
             'P means P+1 pipeline threads, plus monitor, workers and 4 clients. pinned=1 retains the initial snapshot; it does not disable GC.',
             'RSS includes benchmark/reference-model allocations. Retired bytes are cumulative, not peak memory.',
             'Samples run sequentially in randomized order, without CPU affinity. Compare identical settings; incomplete configurations have no performance result.', '',
             '| P | workers | workload | pinned | mode | slots | valid/expected | ms | Mops/s | peak MiB | live MiB | retired MiB | append % |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for s in summaries:
        cells = [str(s[k]) for k in fields[:6]] + [f"{s['valid']}/{s['expected']}"]
        cells += [f'{s[k]:.3f}' if k in s else '—' for k in fields[8:]]
        lines.append('| '+' | '.join(cells)+' |')
    (out/'summary.md').write_text('\n'.join(lines)+'\n')


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--compiler', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--extra-flags', default='', help='Additional include/library/rpath flags')
    parser.add_argument('--records', type=int, default=100000)
    parser.add_argument('--updates', type=int, default=100000)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--warmups', type=int, default=1)
    parser.add_argument('--workers', type=int, nargs='+', default=[1,2,4,8])
    parser.add_argument('--timeout', type=int, default=120, help='Seconds per performance sample')
    parser.add_argument('--smoke', action='store_true', help='Small matrix; correctness gate still runs')
    args = parser.parse_args()
    if min(args.records,args.updates,args.runs,args.timeout,*args.workers)<1 or args.warmups<0:
        parser.error('counts must be positive; warmups must be nonnegative')
    os.chdir(Path(__file__).resolve().parents[1])
    out = Path('build') / ('server-experiment-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid()))
    out.mkdir(parents=True); (out/'bin').mkdir()
    print(f'RESULT_DIR={out.resolve()}', flush=True)
    metadata = dict(arguments=vars(args), platform=platform.platform(), machine=platform.machine(),
                    cpu_count=os.cpu_count(), affinity=sorted(os.sched_getaffinity(0)) if hasattr(os,'sched_getaffinity') else None,
                    started=datetime.datetime.now(datetime.timezone.utc).isoformat(), completed=False,
                    correctness_passed=False, builds=[], tests=[])
    for name, command in [('compiler',[args.compiler,'--version']), ('git_head',['git','rev-parse','HEAD']),
                          ('git_status',['git','status','--short']), ('cpu',['lscpu']), ('memory',['free','-h']),
                          ('load',['uptime'])]:
        metadata[name] = execute(command, 15)
    for file in ('/sys/fs/cgroup/cpu.max','/sys/fs/cgroup/memory.max'):
        if Path(file).exists(): metadata[file]=Path(file).read_text()
    metadata['source_sha256']={str(p):hashlib.sha256(p.read_bytes()).hexdigest()
        for root in ('lib','src','bench','utils') for p in Path(root).rglob('*')
        if p.suffix in ('.hpp','.cpp','.py')}
    rows=[]
    def save():
        (out/'metadata.json').write_text(json.dumps(metadata,indent=2)+'\n')
    def build(name, source, defines):
        command=[args.compiler,source,'-o',str(out/'bin'/name),'-I.','-std=c++20','-O3','-DNDEBUG']
        command += defines+shlex.split(args.extra_flags)+['-pthread','-lmimalloc']
        r=execute(command,600); metadata['builds'].append(r); save()
        (out/(name+'.build.log')).write_text(r['stdout']+r['stderr'])
        if r['status']!='ok': raise RuntimeError(f'Build failed: {name}; see {name}.build.log')
        metadata.setdefault('binary_sha256',{})[name]=hashlib.sha256((out/'bin'/name).read_bytes()).hexdigest()
        return str((out/'bin'/name).resolve())
    try:
        tests=[('cyclic','bench/concow_cyclic_fat_test.cpp',[]),
               ('worker','bench/concow_worker_fat_test.cpp',[]),
               ('gc','bench/concow_cyclic_gc_test.cpp',[]),
               ('ticket','bench/native_ticket_wait_test.cpp',[])]
        tests += [(t,'bench/concow_native_multitree_test.cpp',['-DTREE_'+t.upper()]) for t in ('betree','art','aert')]
        for name,source,defines in tests:
            print(f'[correctness] build/run {name}',flush=True)
            # Ticket test exercises the production ring; other tests deliberately wrap a small ring.
            binary=build(name,source,defines+([] if name=='ticket' else ['-DLIBCONCTRL_BUFFER_SIZE=1024']))
            r=execute([binary],300); metadata['tests'].append(r); save()
            (out/(name+'.test.log')).write_text(r['stdout']+r['stderr'])
            if r['status']!='ok': raise RuntimeError(f'Correctness failed: {name}; performance NOT started')
            print(f'[PASS] {name}',flush=True)
        metadata['correctness_passed']=True;save()
        binaries={p:build(f'btree-p{p}','bench/concow_fat_bench.cpp',
                         ['-DCONCOW_NATIVE=1',f'-DCONCOW_NATIVE_PIPES={p}']) for p in (1,3)}
        configs=list(itertools.product((1,3),args.workers,('uniform','hot'),(False,True),
                    [('baseline',0),('entry',2),('entry',4),('entry',8),('worker',2),('worker',4),('worker',8)]))
        if args.smoke:
            configs=list(itertools.product((1,3),args.workers,('uniform',),(False,True),[('baseline',0),('worker',4)]))
        rng=random.Random(20260922)
        with (out/'samples.jsonl').open('w') as samples:
            total=len(configs)*(args.runs+args.warmups); index=0
            for run in range(1-args.warmups,args.runs+1):
                order=configs.copy();rng.shuffle(order)
                for p,w,pattern,pinned,(mode,slots) in order:
                    command=[binaries[p],'--records',str(args.records),'--updates',str(args.updates),
                             '--clients','4','--workers',str(w),'--slots',str(slots),'--pattern',pattern]
                    if pinned:command+=['--pinned']
                    if mode=='worker':command+=['--fat-workers']
                    row=execute(command,args.timeout)
                    row.update(run=run,p=p,workers=w,pattern=pattern,pinned=int(pinned),mode=mode,slots=slots)
                    if row['status']=='ok':
                        try:
                            m=json.loads(row['stdout']);row['measurement']=m
                            valid=(m['reference_ok'] and m['native_pipeline'] and m['pending_bytes']==0
                                and m['retired_bytes']==m['reclaimed_bytes']
                                and m['appended_updates']+m['cow_updates']==args.updates
                                and m['elapsed_seconds']>0)
                            if mode=='worker':
                                valid=valid and m['worker_appends']==m['appended_updates'] and m['worker_materializations']==m['cow_updates']
                            if not valid:row['status']='invalid_result'
                        except (ValueError,KeyError,TypeError):row['status']='invalid_output'
                    rows.append(row);samples.write(json.dumps(row)+'\n');samples.flush();index+=1
                    print(f'[{index}/{total}] run={run} P={p} w={w} {pattern} pinned={int(pinned)} {mode}{slots}: {row["status"]}',flush=True)
                summarize(out,rows,args.runs)
        metadata['completed']=True
        metadata['failed_samples']=sum(r['status']!='ok' for r in rows)
        print(f'COMPLETE; failed_samples={metadata["failed_samples"]}',flush=True)
        return 0 if metadata['failed_samples']==0 else 2
    except Exception as e:
        metadata['error']=str(e);print(f'ERROR: {e}',flush=True);return 1
    finally:
        save()
        if rows:summarize(out,rows,args.runs)
        archive=out.with_suffix('.tar.gz')
        with tarfile.open(archive,'w:gz') as tar:
            for p in out.iterdir():
                if p.is_file():tar.add(p,arcname=out.name+'/'+p.name)
        print(f'RESULT_ARCHIVE={archive.resolve()}',flush=True)


if __name__=='__main__':
    raise SystemExit(main())
