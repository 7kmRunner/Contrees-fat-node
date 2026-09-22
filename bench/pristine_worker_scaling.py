"""Measure archived, unmodified original B+Tree; reject incorrect samples."""
import csv
import hashlib
import itertools
import json
from pathlib import Path
import platform
import os
import random
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'build/pristine-worker-scaling'
OUT=ROOT/'bench/results/pristine_worker_scaling_20260918.csv'
COMMIT='e487e91d31552685ea4d459223fc78375ed66591'

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    files=subprocess.check_output(['git','ls-tree','-r','--name-only',COMMIT],cwd=ROOT,text=True).splitlines()
    for file in files:
        assert (BASE/file).read_bytes()==subprocess.check_output(['git','show',COMMIT+':'+file],cwd=ROOT),file
    metadata={'commit':COMMIT,'platform':platform.platform(),
              'compiler':subprocess.check_output(['clang++','--version'],text=True),
              'logical_cpu_count':os.cpu_count(),
              'original_sources_unchanged':True,'source_sha256':{f:sha(BASE/f) for f in files},
              'driver_sha256':sha(ROOT/'bench/pristine_worker_scaling.cpp'),
              'script_sha256':sha(Path(__file__)),
              'binaries':{b:sha(BASE/b) for b in ['native-p3','x86-p1','x86-p3']},
              'native_allocator':'mimalloc', 'x86_allocator':'system via benchmark-only include shims',
              'x86_execution':'Rosetta on ARM64, not native x86 hardware',
              'records':100000,'updates':300000,'clients':4,'gc':False,
              'seed':182,'order_seed':20260918,'warmups':1,'runs':3,
              'timing_scope':'submission through completed updates; validation/build excluded',
              'completed':False,'executions':[]}
    meta=OUT.with_suffix('.metadata.json')
    meta.write_text(json.dumps(metadata,indent=2)+'\n')
    fields=['arch','pipes','workers','workload','run','returncode','status','seconds','mismatches','peak_rss_bytes']
    configurations=list(itertools.product([1,3],[1,2,4,8],['update','insert']))
    rng=random.Random(20260918)
    started=time.monotonic()
    with OUT.open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=fields);writer.writeheader()
        # Native compatibility check, not a performance matrix if it fails.
        cases=[('native',3,w,pattern,1) for w in [1,2,4] for pattern in ['update','insert']]
        for run in range(4):
            order=configurations.copy();rng.shuffle(order)
            cases.extend(('x86',p,w,pattern,run) for p,w,pattern in order)
        for arch,pipes,workers,pattern,run in cases:
            cmd=[str(BASE/f'{arch}-p{pipes}'),str(workers),pattern,'100000','300000','4']
            row=dict(arch=arch,pipes=pipes,workers=workers,workload=pattern,run=run)
            try:
                result=subprocess.run(cmd,capture_output=True,text=True,timeout=45)
                row.update(returncode=result.returncode,status='process_failure')
                if result.stdout.strip():
                    data=json.loads(result.stdout)
                    row.update({k:data[k] for k in ['seconds','mismatches','peak_rss_bytes']})
                    row['status']='valid' if result.returncode==0 and data['mismatches']==0 else 'incorrect'
                detail={'command':cmd,'stdout':result.stdout,'stderr':result.stderr}
            except subprocess.TimeoutExpired as error:
                row.update(returncode='timeout',status='timeout')
                detail={'command':cmd,'stdout':str(error.stdout),'stderr':str(error.stderr)}
            writer.writerow(row);f.flush()
            metadata['executions'].append({'row':row,**detail})
            meta.write_text(json.dumps(metadata,indent=2)+'\n')
            print(f'{arch} P={pipes} w={workers} {pattern} run={run} {row["status"]} elapsed={time.monotonic()-started:.1f}s',flush=True)
    metadata['completed']=True
    meta.write_text(json.dumps(metadata,indent=2)+'\n')

if __name__=='__main__':main()
