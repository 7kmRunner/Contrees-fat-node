#!/usr/bin/env python3
"""Bounded fresh-process matrix for experimental ConCow B+Tree, NOT legacy ConCow."""
import argparse
import csv
import json
import subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary',type=Path,default=Path('bench/concow_fat_bench'))
p.add_argument('--records',type=int,default=10000)
p.add_argument('--updates',type=int,default=30000)
p.add_argument('--clients',type=int,default=4)
p.add_argument('--workers',type=int,nargs='+',default=[1,2,4])
p.add_argument('--patterns',nargs='+',choices=['uniform','hot','zipfian'],default=['uniform','hot','zipfian'])
p.add_argument('--parallel-min-groups',type=int,nargs='+',default=[16])
p.add_argument('--materialization',type=int,nargs='+',choices=[0,1],default=[0])
p.add_argument('--runs',type=int,default=3)
p.add_argument('--warmups',type=int,default=1)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
if min(a.records,a.updates,a.clients,a.runs,*a.parallel_min_groups,*a.workers)<=0 or a.warmups<0:p.error('invalid counts')
a.output.parent.mkdir(parents=True,exist_ok=True)
orders=[(0,2,8,4),(2,4,0,8),(4,8,2,0),(8,0,4,2)]
with a.output.open('w',newline='') as out:
    writer=None
    for run in range(-a.warmups,a.runs):
        for pattern_index,pattern in enumerate(a.patterns):
            for pinned in [0,1]:
                workers_order=a.workers if (run+pattern_index+pinned)%2==0 else a.workers[::-1]
                for workers in workers_order:
                    thresholds=a.parallel_min_groups if (run+pinned+workers)%2==0 else a.parallel_min_groups[::-1]
                    for threshold in thresholds:
                        modes=a.materialization if (run+pinned+workers)%2==0 else a.materialization[::-1]
                        for mode in modes:
                            for position,slots in enumerate(orders[(run+pattern_index+pinned+workers)%4]):
                                cmd=[str(a.binary.resolve()),'--records',str(a.records),'--updates',str(a.updates),
                                     '--clients',str(a.clients),'--workers',str(workers),'--slots',str(slots),'--pattern',pattern,
                                     '--parallel-min-groups',str(threshold)]
                                if mode:cmd+=['--parallel-materialization']
                                if pinned:cmd+=['--pinned']
                                result=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
                                if result.returncode:raise RuntimeError(f'{cmd}: {result.returncode}\n{result.stderr}')
                                row=json.loads(result.stdout)
                                if not row['reference_ok'] or row['pending_bytes']:raise RuntimeError('invalid benchmark result')
                                if run>=0:
                                    row={'run':run+1,'order':position+1,**row}
                                    if writer is None:writer=csv.DictWriter(out,fieldnames=row);writer.writeheader()
                                    writer.writerow(row);out.flush()
                        print(f'round={run+1} pattern={pattern} pinned={pinned} workers={workers} threshold={threshold}',flush=True)
