"""Matched four-tree original ConCow comparison; run from repository root."""
import csv, hashlib, json, platform, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'build/multitree-original-baseline'
OUT = ROOT / 'bench/results/multitree_original_current_comparison.csv'
COMMIT = 'e487e91d31552685ea4d459223fc78375ed66591'
TREES = ('btree', 'art', 'aert', 'betree')
def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, **kw)
def prepare():
    BASE.mkdir(parents=True, exist_ok=True)
    run(['git', 'archive', COMMIT, '-o', str(ROOT/'build/multitree-original.tar')])
    run(['tar', '-xf', 'build/multitree-original.tar', '-C', str(BASE)])
    subprocess.run(['patch','-p1','-i',str(ROOT/'bench/btree_original_arm64.patch')],cwd=BASE,check=True)
    subprocess.run(['patch','-p1','-i',str(ROOT/'bench/radix_original_arm64.patch')],cwd=BASE,check=True)
    p=BASE/'lib/trees/betree/node.hpp'
    p.write_text(p.read_text().replace('memcpy(t_new, t, 128);','memcpy(t_new, t, sizeof(node));'))
    for tree in ('art','aert'):
        p=BASE/f'lib/trees/{tree}/node.hpp'
        t=p.read_text().replace('static inline uint8_t prefix_match(cnodeptr t, uint64_t key) {','static inline uint8_t prefix_match(cnodeptr t, uint64_t key) {\n  if (t->pfx_len == 0) return 0;')
        if tree=='art':
            t=t.replace('static inline uint64_t prefix_mask(uint8_t ofs, uint8_t len) {','static inline uint64_t prefix_mask(uint8_t ofs, uint8_t len) {\n  if (len == 0) return 0;')
        p.write_text(t)
    flags=['-I/Users/minrain/.brew/include','-L/Users/minrain/.brew/lib','-Wl,-rpath,/Users/minrain/.brew/lib','-lmimalloc','-pthread','-std=c++20','-O3','-DNDEBUG']
    for tree in TREES:
        for mode in ('baseline','latest'):
            cmd=['clang++','bench/multitree_original_compare.cpp',f'-DTREE_{tree.upper()}','-I'+str(BASE if mode=='baseline' else ROOT)]
            if mode=='baseline':cmd+=['-DORIGINAL_TREE']
            run(cmd+flags+['-o',f'build/multitree_{tree}_{mode}'])
            print(f'built {tree} {mode}',flush=True)
    paths=[ROOT/'bench/multitree_original_compare.cpp',Path(__file__).resolve()]
    paths+=sorted((ROOT/'lib').rglob('*.hpp'))+sorted((ROOT/'src/adapters').glob('*.hpp'))+sorted((BASE/'lib').rglob('*.hpp'))
    paths += [ROOT/f'build/multitree_{t}_{m}' for t in TREES for m in ('baseline','latest')]
    meta={'baseline_commit':COMMIT,'platform':platform.platform(),'compiler':run(['clang++','--version'],capture_output=True).stdout,'flags':flags,'sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}}
    OUT.with_suffix('.metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
def measure(tree,mode,workers,workload,records,updates):
    cmd=[f'build/multitree_{tree}_{mode}',str(workers),workload,str(records),str(updates)]
    result=subprocess.run(cmd,cwd=ROOT,text=True,capture_output=True,timeout=60)
    if result.returncode not in (0,3):
        raise RuntimeError(f'{cmd}: {result.returncode} {result.stderr}')
    row={'tree':tree,'mode':mode,'returncode':result.returncode,**json.loads(result.stdout)}
    if mode=='latest' and row['mismatches']:raise RuntimeError(row)
    return row
if __name__=='__main__':
    prepare()
    for tree in TREES:
        for mode in ('baseline','latest'):
            for workload in ('update','insert'):
                row=measure(tree,mode,2,workload,10000,10000)
                print(f'pilot {tree} {mode} {workload}: mismatches={row["mismatches"]}',flush=True)
        print(f'pilot finished {tree}',flush=True)
    with OUT.open('w',newline='') as stream:
        writer=None
        for repeat in range(4):
            for tree in TREES:
                for workload in ('update','insert'):
                    configs=[(m,w) for w in (1,2,4) for m in ('baseline','latest')]
                    if repeat%2:configs.reverse()
                    for mode,workers in configs:
                        row={'run':repeat,**measure(tree,mode,workers,workload,100000,300000)}
                        if repeat:
                            if writer is None:
                                writer=csv.DictWriter(stream,fieldnames=row.keys());writer.writeheader()
                            writer.writerow(row);stream.flush()
                    print(f'run={repeat} tree={tree} workload={workload}',flush=True)
