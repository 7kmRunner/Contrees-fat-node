"""Diagnostic CPU sampling of the update phase, not a replacement for paper timing."""
import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import random
import shutil
import struct
import tarfile
from paper_pair import ROOT, run, digest, write_spec


def main():
    ap=argparse.ArgumentParser(__doc__)
    ap.add_argument('--source-run',type=Path,help='Completed three-mode paper-pair result directory')
    ap.add_argument('--runs',type=int,default=3)
    ap.add_argument('--frequency',type=int,default=199)
    ap.add_argument('--smoke',action='store_true',help='100k/100k, diagnostic only')
    ap.add_argument('--timeout',type=int,default=1800)
    a=ap.parse_args()
    if min(a.runs,a.frequency,a.timeout)<1:ap.error('counts must be positive')
    if platform.system()!='Linux' or platform.machine()!='x86_64':ap.error('Linux x86-64 required')
    os.chdir(ROOT)
    if a.source_run:
        source=a.source_run.resolve()
    else:
        candidates=[]
        for path in (ROOT/'build').glob('paper-pair-*/metadata.json'):
            try:
                m=json.loads(path.read_text())
                if m.get('completed') and m.get('arguments',{}).get('include_worker') and not m.get('failed_samples') and not m.get('failed_preflights'):
                    candidates.append(path.parent)
            except (ValueError,OSError):pass
        if not candidates:ap.error('No completed three-mode experiment found; use --source-run')
        source=sorted(candidates)[-1]
    out=ROOT/'build'/('paper-profile-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+str(os.getpid()))
    out.mkdir(parents=True)
    for name in ('logs','profiles','bin'): (out/name).mkdir()
    meta=dict(arguments={**vars(a),'source_run':str(source)},completed=False,diagnostic_only=True,
              sampler='cpu-clock:u',scope='second timer interval: submission/execution/commit wait',
              runner_sha256=digest(Path(__file__)),gate_sha256=digest(ROOT/'bench/profile_gate.hpp'),
              environment={},builds=[],measurements=[])
    print('RESULT_DIR='+str(out),flush=True)
    def save(): (out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
    def execute(cmd,cwd,timeout,label,required=True):
        r=run(cmd,cwd,timeout);(out/'logs'/(label+'.log')).write_text(r['stdout']+r['stderr'])
        if required and r['status']!='ok':raise RuntimeError(label+': '+r['status']+'; see logs/'+label+'.log')
        return r
    try:
        previous=json.loads((source/'metadata.json').read_text())
        if not previous.get('completed') or not previous.get('arguments',{}).get('include_worker'):
            raise RuntimeError('Source run must be a completed three-mode experiment')
        meta['baseline_metadata']=previous
        for name,cmd in [('perf',['perf','--version']),('compiler',[previous['arguments']['compiler'],'--version']),
                         ('cpu',['lscpu']),('load',['uptime'])]:
            meta['environment'][name]=execute(cmd,ROOT,20,name,False)
        paranoid=Path('/proc/sys/kernel/perf_event_paranoid')
        if paranoid.exists():meta['perf_event_paranoid']=paranoid.read_text().strip()
        save()
        # Fail before expensive compilation/data generation if perf is unavailable or forbidden.
        execute(['perf','record','-e','cpu-clock:u','-o',str(out/'profiles/permission.data'),'--','/bin/true'],ROOT,20,'perf-permission')
        compiler=previous['arguments']['compiler']
        import shlex
        extra=shlex.split(previous['arguments'].get('extra_flags',''))
        for variant in ('original','fat'):
            src=source/(variant+'-source')
            hashes=previous[variant+'-source_files_sha256']
            for relative,expected in hashes.items():
                if digest(src/relative)!=expected:raise RuntimeError('Source changed: '+str(src/relative))
            wrapper=out/(variant+'_profile.cpp')
            wrapper.write_text('#include "'+str(ROOT/'bench/profile_gate.hpp')+'"\n#define timer profile_timer\n#include "'+str(src/'src/main.cpp')+'"\n')
            cmd=[compiler,str(wrapper),'-o',str(out/'bin'/variant),'-I'+str(src),'-std=c++20','-O3','-DNDEBUG',
                 '-g','-fno-omit-frame-pointer','-march=native','-msse4','-maes',*extra,'-pthread','-lmimalloc']
            print('[build] '+variant,flush=True)
            meta['builds'].append(execute(cmd,ROOT,900,'build-'+variant))
            meta.setdefault('binary_sha256',{})[variant]=digest(out/'bin'/variant);save()
        # Exact original generator, same workload as the successful insertion experiment.
        gen=out/'generator';shutil.copytree(source/'original-source/ycsbc',gen)
        (gen/'export').mkdir(exist_ok=True)
        sources=sorted(gen.glob('*.cc'))+sorted((gen/'core').glob('*.cc'))+sorted((gen/'db').glob('*.cc'))
        execute([compiler,*map(str,sources),'-o',str(out/'bin/ycsbc'),'-I'+str(gen),'-std=c++20','-O3','-DNDEBUG','-pthread'],ROOT,600,'build-generator')
        n,m=(100000,100000) if a.smoke else (100000000,1000000)
        meta.update(records=n,operations=m,runs=a.runs,clients=32,p=1,workers=8,slots=previous['arguments']['slots'],gc=False)
        write_spec(gen/'workloads/workload_update.spec',gen/'workloads/profile.spec',n,m)
        shutil.copyfile(gen/'workloads/profile.spec',out/'workload.spec')
        print(f'[generate] records={n} operations={m}',flush=True)
        execute([str(out/'bin/ycsbc'),'-db','export','-P','profile'],gen,3600,'generate')
        data=gen/'export/profile.data'
        with data.open('rb') as f:assert struct.unpack('<QQ',f.read(16))==(n,m)
        assert data.stat().st_size==16+n*8+m*24
        meta['dataset_sha256']=digest(data);save()
        rng=random.Random(20260923)
        for repeat in range(1,a.runs+1):
            order=['original','fat','worker'];rng.shuffle(order)
            for variant in order:
                label=f'r{repeat}-{variant}';ctl=out/(label+'.ctl');ack=out/(label+'.ack')
                os.mkfifo(ctl);os.mkfifo(ack)
                binary=out/'bin'/('fat' if variant=='worker' else variant)
                command=['perf','record','--delay=-1','--control=fifo:'+str(ctl)+','+str(ack),
                         '-e','cpu-clock:u','-F',str(a.frequency),'--call-graph','fp',
                         '-o',str(out/'profiles'/(label+'.data')),'--','env',
                         'CONTREES_PERF_CTL='+str(ctl),'CONTREES_PERF_ACK='+str(ack),
                         str(binary),str(data),'concow','btree','-c','32','-p','1','-w','8']
                if variant!='original':command+=['--fat-slots',str(meta['slots'])]
                if variant=='worker':command+=['--fat-workers']
                print(f'[profile {repeat}/{a.runs}] {variant}',flush=True)
                r=execute(command,ROOT,a.timeout,label)
                if r['stderr'].count('[PROFILE] enable\n')!=1 or r['stderr'].count('[PROFILE] disable\n')!=1:
                    raise RuntimeError('Invalid phase gate: '+label)
                meta['measurements'].append(dict(run=repeat,variant=variant,**r));save()
                ctl.unlink();ack.unlink()
                perfdata=str(out/'profiles'/(label+'.data'))
                for suffix,options in [('self',['--no-children','--sort','symbol,dso']),
                                       ('callers',['--children','--sort','symbol']),
                                       ('lines',['--no-children','--sort','symbol,srcline'])]:
                    result=execute(['perf','report','--stdio','--stdio-color','never','--percent-limit','0.5','-i',perfdata,*options],ROOT,180,label+'-'+suffix)
                    (out/'profiles'/(label+'-'+suffix+'.txt')).write_text(result['stdout']+result['stderr'])
                script=execute(['perf','script','-i',perfdata,'-F','comm,pid,tid,time,event,ip,sym,dso'],ROOT,180,label+'-stacks')
                if not script['stdout'].strip():raise RuntimeError('No CPU samples captured: '+label)
                (out/'profiles'/(label+'-stacks.txt')).write_text(script['stdout'])
        meta['completed']=True
        print('COMPLETE: diagnostic profiles only; do not use these timings as uninstrumented throughput.',flush=True)
        return 0
    except Exception as e:
        meta['error']=str(e);print('ERROR: '+str(e),flush=True);return 1
    finally:
        save()
        archive=out.with_suffix('.tar.gz')
        with tarfile.open(archive,'w:gz') as t:
            for p in out.rglob('*'):
                if p.is_file() and (p.parent==out or p.parent.name in ('logs','profiles')):
                    t.add(p,arcname=str(Path(out.name)/p.relative_to(out)))
        print('RESULT_ARCHIVE='+str(archive),flush=True)


if __name__=='__main__':raise SystemExit(main())
