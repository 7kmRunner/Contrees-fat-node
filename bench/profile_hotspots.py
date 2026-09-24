"""Resolve existing perf samples to a small set of hot instructions; runs no workload."""
import argparse
import bisect
from collections import Counter, defaultdict
import csv
import datetime
import json
from pathlib import Path
import re
import struct
import tarfile
from paper_pair import ROOT, digest, run


def mappings(data):
    if data[:8]!=b'PERFILE2':raise ValueError('Expected little-endian perf.data')
    off,size=struct.unpack_from('<QQ',data,40);end=off+size;result=[]
    if end>len(data):raise ValueError('Truncated perf data section')
    while off<end:
        kind,misc,length=struct.unpack_from('<IHH',data,off)
        if length<8 or off+length>end:raise ValueError('Invalid perf record size')
        if kind in (1,10):
            pid,tid,addr,span,pgoff=struct.unpack_from('<IIQQQ',data,off+8)
            start=off+(40 if kind==1 else 72)
            filename=data[start:off+length].split(b'\0')[0].decode(errors='replace')
            result.append((pid,addr,span,pgoff,filename))
        off+=length
    return result


def samples(text):
    for block in text.strip().split('\n\n'):
        lines=block.splitlines()
        if len(lines)<2:continue
        header=re.search(r'\s(\d+)/(\d+)\s+[\d.]+:\s+cpu-clock',lines[0])
        frame=re.match(r'\s*([0-9a-fA-F]+)\s+(.*?)\s+\((.*)\)\s*$',lines[1])
        if header and frame:yield int(header[1]),int(frame[1],16),frame[2],frame[3]


def category(symbol):
    if 'multi_client_execute' in symbol:return 'client'
    if 'worker_main_' in symbol:return 'worker'
    if 'monitor_main_' in symbol:return 'monitor'
    if 'pipe_main_' in symbol:return 'pipeline'
    if 'pipeline_fat_' in symbol:return 'fat_probe'
    if 'update_cow' in symbol:return 'cow'
    return 'other'


def file_to_vma(binary,offset):
    if binary[:6]!=b'\x7fELF\x02\x01':raise ValueError('Expected ELF64 little-endian')
    phoff=struct.unpack_from('<Q',binary,32)[0]
    entsize,count=struct.unpack_from('<HH',binary,54)
    for i in range(count):
        kind,flags,fileoff,vaddr,paddr,filesize,memsize,align=struct.unpack_from('<IIQQQQQQ',binary,phoff+i*entsize)
        if kind==1 and fileoff<=offset<fileoff+filesize:return vaddr+offset-fileoff
    raise ValueError('Sample outside ELF load segments')


def collect(profile):
    counts=defaultdict(Counter);totals=Counter();unmapped=Counter()
    for path in sorted((profile/'profiles').glob('r*-stacks.txt')):
        label=path.name.removesuffix('-stacks.txt');variant=label.split('-',1)[1]
        maps=mappings((profile/'profiles'/(label+'.data')).read_bytes())
        for pid,ip,symbol,dso in samples(path.read_text()):
            totals[variant]+=1
            expected='original' if variant=='original' else 'fat'
            if Path(dso).name!=expected:continue
            matches=[m for m in maps if m[0]==pid and m[1]<=ip<m[1]+m[2] and m[4]==dso]
            if len(matches)!=1:unmapped[variant]+=1;continue
            m=matches[0];offset=ip-m[1]+m[3]
            counts[variant][(offset,symbol,category(symbol))]+=1
    if not totals:raise ValueError('No decoded samples found')
    return counts,totals,unmapped


def main():
    ap=argparse.ArgumentParser(__doc__)
    ap.add_argument('profile',type=Path)
    ap.add_argument('--top',type=int,default=3,help='Hot instructions per category per variant')
    ap.add_argument('--counts-only',action='store_true',help='No ELF files or binutils needed')
    a=ap.parse_args()
    if a.top<1:ap.error('--top must be positive')
    profile=a.profile.resolve();source=json.loads((profile/'metadata.json').read_text())
    out=ROOT/'build'/('profile-hotspots-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True,exist_ok=False)
    meta=dict(profile=str(profile),completed=False,counts_only=a.counts_only,commands=[])
    print('RESULT_DIR='+str(out),flush=True)
    try:
        counts,totals,unmapped=collect(profile)
        meta.update(total_samples=dict(totals),unmapped_binary_samples=dict(unmapped))
        selected=[];rows=[]
        for variant,counter in counts.items():
            bycat=defaultdict(list)
            for (offset,symbol,cat),n in counter.most_common():
                row=dict(variant=variant,category=cat,file_offset=hex(offset),samples=n,
                         percent=100*n/totals[variant],symbol=symbol)
                rows.append(row);bycat[cat].append(row)
            for group in bycat.values():selected+=group[:a.top]
        with (out/'hotspots.csv').open('w',newline='') as f:
            writer=csv.DictWriter(f,fieldnames=['variant','category','file_offset','samples','percent','symbol']);writer.writeheader();writer.writerows(rows)
        (out/'selected.json').write_text(json.dumps(selected,indent=2)+'\n')
        if not a.counts_only:
            for name in ('original','fat'):
                binary=profile/'bin'/name
                actual=digest(binary)
                if actual!=source['binary_sha256'][name]:raise ValueError('Binary hash mismatch: '+name)
                data=binary.read_bytes()
                chosen=[r for r in selected if (r['variant']=='original')==(name=='original')]
                for r in chosen:r['vma']=hex(file_to_vma(data,int(r['file_offset'],16)))
                addresses=sorted({int(r['vma'],16) for r in chosen})
                cmd=['objdump','-d','-C','--no-show-raw-insn',str(binary)]
                result=run(cmd,ROOT,60);meta['commands'].append(dict(command=cmd,status=result['status']))
                if result['status']!='ok':raise RuntimeError('objdump failed: '+result['stderr'])
                lines=result['stdout'].splitlines();positions=[];instructions=[]
                for i,line in enumerate(lines):
                    match=re.match(r'\s*([0-9a-f]+):\s',line)
                    if match:positions.append(int(match[1],16));instructions.append(i)
                # objdump text sections need not be ordered globally.
                lookup=sorted(zip(positions,instructions));ordered=[x[0] for x in lookup]
                report=[]
                for address in addresses:
                    index=bisect.bisect_left(ordered,address)
                    if index==len(ordered) or ordered[index]!=address:
                        report.append(f'UNRESOLVED instruction boundary {address:#x}');continue
                    line=lookup[index][1]
                    tags=[f"{r['variant']} {r['category']} samples={r['samples']} percent={r['percent']:.3f}" for r in chosen if int(r['vma'],16)==address]
                    report+=['\nHOT '+hex(address)+' '+ '; '.join(tags),'\n'.join(lines[max(0,line-12):line+14])]
                (out/(name+'-instructions.txt')).write_text('\n'.join(report)+'\n')
                # Resolve only the selected addresses, never the whole sample stream.
                cmd=['addr2line','-a','-f','-C','-i','-e',str(binary),*[hex(v) for v in addresses]]
                result=run(cmd,ROOT,30);meta['commands'].append(dict(command=cmd,status=result['status']))
                (out/(name+'-source-lines.txt')).write_text(result['stdout']+result['stderr'])
                if result['status']!='ok':print('[WARN] limited addr2line failed; instruction report retained',flush=True)
                print('[DONE] '+name+' hot instruction report',flush=True)
            (out/'selected.json').write_text(json.dumps(selected,indent=2)+'\n')
        meta['completed']=True;print('COMPLETE: existing samples analyzed; no benchmark was run.',flush=True)
        return 0
    except Exception as e:
        meta['error']=str(e);print('ERROR: '+str(e),flush=True);return 1
    finally:
        (out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
        archive=out.with_suffix('.tar.gz')
        with tarfile.open(archive,'w:gz') as t:
            for path in out.iterdir():
                if path.is_file():t.add(path,arcname=out.name+'/'+path.name)
        print('RESULT_ARCHIVE='+str(archive),flush=True)


if __name__=='__main__':raise SystemExit(main())
