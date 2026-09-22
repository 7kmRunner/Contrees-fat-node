"""Run pristine upstream CLI and fat-node CLI with paper Section 6 workloads."""
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
import re
import shlex
import shutil
import signal
import statistics
import struct
import subprocess
import tarfile
import time
from collections import defaultdict

ORIGINAL='e487e91d31552685ea4d459223fc78375ed66591'
BEST={'btree':(1,8),'art':(1,8),'betree':(3,16),'aert':(3,16)}
ROOT=Path(__file__).resolve().parents[1]


def run(command, cwd, timeout):
    start=time.monotonic()
    try:
        p=subprocess.Popen(command,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                           text=True,start_new_session=True,env={**os.environ,'LC_ALL':'C'})
        try:
            stdout,stderr=p.communicate(timeout=timeout)
            status='ok' if p.returncode==0 else 'process_failure'
        except subprocess.TimeoutExpired:
            os.killpg(p.pid,signal.SIGKILL);stdout,stderr=p.communicate();status='timeout'
        return dict(command=list(map(str,command)),status=status,returncode=p.returncode,
                    stdout=stdout,stderr=stderr,wall_seconds=time.monotonic()-start)
    except OSError as e:
        return dict(command=list(map(str,command)),status='launch_failure',stdout='',stderr=str(e))


def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for chunk in iter(lambda:f.read(4*1024*1024),b''):h.update(chunk)
    return h.hexdigest()


def write_spec(original, target, records, operations):
    text=original.read_text()
    for key,value in [('recordcount',records),('operationcount',operations)]:
        text,count=re.subn(r'^'+key+r'=\d+\s*$',key+'='+str(value),text,flags=re.M)
        if count!=1:raise ValueError('invalid spec: '+str(original))
    target.write_text(text)


def report(out, rows, repeats):
    groups=defaultdict(list)
    for r in rows:groups[(r['tree'],r['workload'],r['p'],r['workers'])].append(r)
    table=[]
    for key, samples in sorted(groups.items()):
        row=dict(zip(('tree','workload','p','workers'),key))
        for variant in ('original','fat'):
            batch=[r for r in samples if r['variant']==variant]
            good=[r for r in batch if r['status']=='ok' and 'process_seconds' in r]
            row[variant+'_valid']=f'{len(good)}/{repeats}'
            if len(good)==repeats:
                row[variant+'_mean_mops']=statistics.mean(r['mops'] for r in good)
                row[variant+'_mean_peak_mib']=statistics.mean(r['peak_rss_kib']/1024 for r in good)
                row[variant+'_mops_stdev']=statistics.stdev(r['mops'] for r in good) if repeats>1 else 0
        safe=all(r.get('preflight_ok') and not r.get('timing_warning') for r in samples)
        row['comparison_eligible']=safe and all(variant+'_mean_mops' in row for variant in ('original','fat'))
        if row['comparison_eligible']:
            row['fat_speedup']=row['fat_mean_mops']/row['original_mean_mops']
        table.append(row)
    fields=['tree','workload','p','workers','original_valid','fat_valid','original_mean_mops',
            'fat_mean_mops','fat_speedup','original_mean_peak_mib','fat_mean_peak_mib',
            'original_mops_stdev','fat_mops_stdev','comparison_eligible']
    with (out/'summary.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(table)
    lines=['# Paper-workload two-version comparison','',
           'Original CLI source is unchanged. Both variants disable GC; fat uses entry append.',
           'MOPS is the arithmetic mean of per-run throughput. Peak RSS includes loading/building and is NOT paper Fig. 13 tree-only memory.',
           'Small preflight verifies final values only, not full-scale concurrent query/scan results.',
           'Original BeTree operation-count waiting can end timing early at checkpoints; its speedup is always withheld. Other observed ticket gaps also suppress speedup.',
           'This compares two concurrent implementations, not every baseline or figure in the paper. See metadata and docs/PAPER_PAIR.md.','',
           '| tree | workload | P | workers | original valid | fat valid | original MOPS | fat MOPS | fat/original | eligible |',
           '|---|---|---|---|---|---|---|---|---|---|']
    for r in table:
        cells=[str(r[k]) for k in fields[:6]]
        cells += [f'{r[k]:.4f}' if k in r else '—' for k in ('original_mean_mops','fat_mean_mops','fat_speedup')]
        cells += [str(r['comparison_eligible'])]
        lines.append('| '+' | '.join(cells)+' |')
    (out/'summary.md').write_text('\n'.join(lines)+'\n')


def main():
    ap=argparse.ArgumentParser(__doc__)
    ap.add_argument('--suite',choices=['insert','mixed','scaling'],default='insert')
    ap.add_argument('--trees',nargs='+',choices=list(BEST),default=list(BEST))
    ap.add_argument('--slots',type=int,choices=[2,4,8],default=8)
    ap.add_argument('--compiler',default='g++')
    ap.add_argument('--extra-flags',default='')
    ap.add_argument('--timeout',type=int,default=1800)
    ap.add_argument('--smoke',action='store_true',help='100k records/100k operations/1 run; NOT paper scale')
    a=ap.parse_args()
    if a.timeout<1:ap.error('timeout must be positive')
    if platform.system()!='Linux' or platform.machine() not in ('x86_64','AMD64'):
        ap.error('Run this experiment on the Linux x86-64 server.')
    if not Path('/usr/bin/time').exists():ap.error('Install GNU time first (Ubuntu: sudo apt-get install time)')
    os.chdir(ROOT)
    out=ROOT/'build'/('paper-pair-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid()))
    out.mkdir(parents=True);(out/'bin').mkdir();(out/'logs').mkdir();(out/'specs').mkdir()
    print('RESULT_DIR='+str(out),flush=True)
    n,m,repeats=(100000,100000,1) if a.smoke else (100000000,1000000,10)
    meta=dict(arguments=vars(a),records=n,operations=m,repeats=repeats,clients=32,gc=False,
              original_commit=ORIGINAL,runner_sha256=digest(Path(__file__)),preflight_source_sha256=digest(ROOT/'bench/paper_preflight.cpp'),paper_scale=not a.smoke,completed=False,builds=[],preflight=[],datasets=[],
              hardware=run(['lscpu'],ROOT,15),memory=run(['free','-h'],ROOT,15),
              compiler=run([a.compiler,'--version'],ROOT,15),load=run(['uptime'],ROOT,15),
              affinity=sorted(os.sched_getaffinity(0)),platform=platform.platform(),
              git_status=run(['git','status','--short'],ROOT,15))
    rows=[]
    def save(): (out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
    def required(command,cwd,timeout,log):
        r=run(command,cwd,timeout);(out/'logs'/log).write_text(r['stdout']+r['stderr'])
        if r['status']!='ok':raise RuntimeError(log+': '+r['status'])
        return r
    def archive_source(ref,name):
        target=out/name;target.mkdir();archive=out/(name+'.tar')
        required(['git','archive',ref,'-o',str(archive)],ROOT,60,name+'-archive.log')
        hashes={}
        with tarfile.open(archive) as t:
            for member in t.getmembers():
                path=Path(member.name)
                if path.is_absolute() or '..' in path.parts or member.issym() or member.islnk():
                    raise ValueError('unexpected archive entry')
                t.extract(member,target)
                if member.isfile():hashes[member.name]=digest(target/member.name)
        meta[name+'_files_sha256']=hashes
        return target
    try:
        meta['fat_commit']=required(['git','rev-parse','HEAD'],ROOT,15,'head.log')['stdout'].strip()
        original=archive_source(ORIGINAL,'original-source')
        fat=archive_source(meta['fat_commit'],'fat-source')
        extra=shlex.split(a.extra_flags)
        binaries={};checks={}
        for variant,source in [('original',original),('fat',fat)]:
            for check in (False,True):
                name=variant+('-check' if check else '')
                src=ROOT/'bench/paper_preflight.cpp' if check else source/'src/main.cpp'
                command=[a.compiler,str(src),'-o',str(out/'bin'/name),'-I'+str(source),
                         '-std=c++20','-O3','-DNDEBUG','-march=native','-msse4','-maes']
                if check and variant=='fat':command+=['-DPAPER_FAT']
                command+=extra+['-pthread','-lmimalloc']
                print('[build] '+name,flush=True)
                r=required(command,source,900,name+'-build.log');meta['builds'].append(r)
                (checks if check else binaries)[variant]=str(out/'bin'/name)
                meta.setdefault('binary_sha256',{})[name]=digest(out/'bin'/name);save()
        # Generator and its templates come exclusively from the original commit.
        gen=out/'generator';shutil.copytree(original/'ycsbc',gen);(gen/'export').mkdir(exist_ok=True)
        sources=sorted(str(p) for p in gen.glob('*.cc'))+sorted(str(p) for p in (gen/'core').glob('*.cc'))+sorted(str(p) for p in (gen/'db').glob('*.cc'))
        required([a.compiler,*sources,'-o',str(out/'bin/ycsbc'),'-I'+str(gen),'-std=c++20','-O3','-DNDEBUG','-pthread'],gen,600,'generator-build.log')
        meta['generator_sha256']=digest(out/'bin/ycsbc')
        def generate(workload,records,operations,label):
            name='pair_'+label;spec=gen/'workloads'/(name+'.spec')
            write_spec(original/'ycsbc/workloads'/(workload+'.spec'),spec,records,operations)
            shutil.copyfile(spec,out/'specs'/(name+'.spec'))
            required([str(out/'bin/ycsbc'),'-db','export','-P',name],gen,3600,name+'-generate.log')
            data=gen/'export'/(name+'.data')
            with data.open('rb') as f:counts=struct.unpack('<QQ',f.read(16))
            if counts!=(records,operations) or data.stat().st_size!=16+records*8+operations*24:
                raise RuntimeError('dataset header/size mismatch')
            meta['datasets'].append(dict(label=label,workload=workload,records=records,operations=operations,
                                         sha256=digest(data),spec_sha256=digest(spec)))
            save();return data
        workloads=['workload_update'] if a.suite!='mixed' else (
            ['workload_lookup'+str(v) for v in (10,30,50,70,90)]+
            ['workload_scan'+str(v) for v in (10,20,30,40,50)])
        configs=[(tree,*BEST[tree]) for tree in a.trees] if a.suite!='scaling' else list(itertools.product(a.trees,range(1,6),(2,4,8,16,32)))
        rng=random.Random(20260922);gate={}
        # Cross the production context ring (65536). All variants use identical input.
        for workload in workloads:
            data=generate(workload,100000,100000,'preflight_'+workload)
            for tree,p,w in configs:
                for variant in ('original','fat'):
                    command=[checks[variant],str(data),'concow',tree,'-c','32','-p',str(p),'-w',str(w)]
                    if variant=='fat':command+=['--fat-slots',str(a.slots)]
                    result=run(command,ROOT,a.timeout)
                    try: result['verification']=json.loads(result['stdout'])
                    except ValueError:result['verification']={}
                    ok=result['status']=='ok' and result['verification'].get('mismatches')==0
                    gap=result['verification'].get('ticket_gap',False)
                    gate[(workload,tree,p,w,variant)]=(ok,gap)
                    result.update(workload=workload,tree=tree,p=p,workers=w,variant=variant)
                    meta['preflight'].append(result);save()
                    print(f'[preflight] {workload} {tree} P={p} w={w} {variant}: '+('PASS' if ok else 'FAIL')+(' ticket-gap' if gap else ''),flush=True)
            data.unlink()
        with (out/'samples.jsonl').open('w') as f:
            for repeat in range(1,repeats+1):
                order=workloads.copy();rng.shuffle(order)
                for workload in order:
                    eligible=[c for c in configs if all(gate[(workload,*c,v)][0] for v in ('original','fat'))]
                    if not eligible:
                        print('[SKIP] no correctness-passing pair: '+workload,flush=True);continue
                    print(f'[generate] run={repeat}/{repeats} {workload} n={n} ops={m}',flush=True)
                    data=generate(workload,n,m,f'run{repeat}_'+workload)
                    rng.shuffle(eligible)
                    for tree,p,w in eligible:
                        variants=['original','fat'];rng.shuffle(variants)
                        for variant in variants:
                            label=f'r{repeat}_{workload}_{tree}_p{p}_w{w}_{variant}'
                            rss=out/'logs'/(label+'.time')
                            command=['/usr/bin/time','-v','-o',str(rss),binaries[variant],str(data),'concow',tree,
                                     '-c','32','-p',str(p),'-w',str(w)]
                            if variant=='fat':command+=['--fat-slots',str(a.slots)]
                            r=run(command,ROOT,a.timeout)
                            (out/'logs'/(label+'.log')).write_text(r['stdout']+r['stderr'])
                            r.update(run=repeat,workload=workload,tree=tree,p=p,workers=w,variant=variant,
                                     preflight_ok=True,timing_warning=(tree=='betree' or gate[(workload,tree,p,w,'original')][1]))
                            times=re.findall(r'process time \(s\):\s*([0-9.eE+-]+)',r['stdout']+'\n'+r['stderr'])
                            peak=re.findall(r'Maximum resident set size \(kbytes\):\s*(\d+)',rss.read_text() if rss.exists() else '')
                            if r['status']=='ok':
                                if len(times)!=1 or not peak or float(times[0])<=0:r['status']='invalid_output'
                                else:r.update(process_seconds=float(times[0]),mops=m/float(times[0])/1e6,peak_rss_kib=int(peak[0]))
                            rows.append(r);f.write(json.dumps(r)+'\n');f.flush()
                            print(f'[run {repeat}/{repeats}] {tree} {workload} P={p} w={w} {variant}: {r["status"]}',flush=True)
                    data.unlink();report(out,rows,repeats)
        meta['completed']=True
        meta['failed_preflights']=sum(not v[0] for v in gate.values())
        meta['failed_samples']=sum(r['status']!='ok' for r in rows)
        meta['timing_warning']=('betree' in a.trees or any(v[1] for k,v in gate.items() if k[-1]=='original'))
        print(f'COMPLETE; failed_preflights={meta["failed_preflights"]}; failed_samples={meta["failed_samples"]}; timing_warning={meta["timing_warning"]}',flush=True)
        return 0 if not(meta['failed_preflights'] or meta['failed_samples'] or meta['timing_warning']) else 2
    except Exception as e:
        meta['error']=str(e);print('ERROR: '+str(e),flush=True);return 1
    finally:
        save();report(out,rows,repeats)
        archive=out.with_suffix('.tar.gz')
        with tarfile.open(archive,'w:gz') as t:
            for p in out.rglob('*'):
                if p.is_file() and (p.parent==out and p.suffix in ('.json','.jsonl','.csv','.md') or p.parent.name in ('logs','specs')):
                    t.add(p,arcname=str(Path(out.name)/p.relative_to(out)))
        print('RESULT_ARCHIVE='+str(archive),flush=True)


if __name__=='__main__':raise SystemExit(main())
