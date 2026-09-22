"""Paired before/after materialization and cyclic measurements (no concurrent runs)."""
import csv
import hashlib
import itertools
import json
import platform
from pathlib import Path
import random
import subprocess

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/fat-merge-improvement'
OUT=ROOT/'bench/results/fat_merge_improvement_20260918'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    meta={'platform':platform.platform(),'seed':20260918,'runs':5,'warmups':1,
          'before_header_sha256':sha(BUILD/'update_cow.before.hpp'),
          'after_header_sha256':sha(ROOT/'lib/trees/btree/update_cow.hpp'),
          'binary_sha256':{p.name:sha(p) for p in BUILD.iterdir() if p.name in ['before-p1','before-p3','after-p1','after-p3','micro-before','micro-after']},
          'driver_sha256':{p:sha(ROOT/p) for p in ['bench/concow_fat_bench.cpp','bench/btree_materialize_bench.cpp','bench/fat_merge_improvement.py']},
          'sequential':True,'gc':True,'pinned':False,'clients':4,'records':100000,'updates':300000,
          'commands':[],'completed':False}
    mp=OUT.with_suffix('.metadata.json');mp.write_text(json.dumps(meta,indent=2)+'\n')
    rng=random.Random(meta['seed'])
    for kind in ['micro','cyclic']:
        configs=list(itertools.product([2,4,8],['entry','worker'],['update','insert'])) if kind=='micro' else list(itertools.product([1,3],[1,2],['uniform','hot'],[(0,False),(2,False),(2,True),(4,False),(4,True),(8,False),(8,True)]))
        with Path(str(OUT)+'_'+kind+'.csv').open('w',newline='') as out:
            writer=None
            for run in range(6):
                order=configs.copy();rng.shuffle(order)
                for config in order:
                    versions=['before','after'];rng.shuffle(versions)
                    for version in versions:
                        if kind=='micro':
                            slots,mode,pattern=config
                            command=[str(BUILD/('micro-'+version)),str(slots),mode,pattern,'500000']
                        else:
                            pipes,workers,pattern,(slots,worker)=config
                            command=[str(BUILD/f'{version}-p{pipes}'),'--records','100000','--updates','300000','--clients','4','--workers',str(workers),'--slots',str(slots),'--pattern',pattern]
                            if worker:command.append('--fat-workers')
                        result=subprocess.run(command,capture_output=True,text=True,timeout=120,check=True)
                        row=json.loads(result.stdout)
                        assert row['reference_ok']
                        if kind=='cyclic':
                            assert row['pending_bytes']==0 and row['retired_bytes']==row['reclaimed_bytes']
                            assert row['appended_updates']+row['cow_updates']==300000
                        meta['commands'].append({'kind':kind,'run':run,'version':version,'command':command})
                        if run:
                            row={'run':run,'version':version,**row}
                            if writer is None:writer=csv.DictWriter(out,fieldnames=row);writer.writeheader()
                            writer.writerow(row);out.flush()
                mp.write_text(json.dumps(meta,indent=2)+'\n')
                print(kind,'run',run,'complete',flush=True)
    meta['completed']=True;mp.write_text(json.dumps(meta,indent=2)+'\n')
if __name__=='__main__':main()
