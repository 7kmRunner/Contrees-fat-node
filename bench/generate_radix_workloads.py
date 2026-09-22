#!/usr/bin/env python3
"""Deterministic matrix, including finite Zipf(theta=0.99) over permuted keys."""
import argparse
import bisect
import itertools
import random
import struct
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('output',type=Path)
parser.add_argument('--records',type=int,default=10000)
parser.add_argument('--operations',type=int,default=None)
parser.add_argument('--scenes',nargs='+',choices=['uniform','hot','lookup90','long','zipfian'],default=['uniform','hot','lookup90','long'])
a=parser.parse_args()
if a.records <= 0 or (a.operations is not None and a.operations <= 0):
    parser.error('record and operation counts must be positive')
n=a.records
ops=a.operations or 5*n
a.output.mkdir(parents=True,exist_ok=True)
r=random.Random(182)
# Inverse CDF of P(rank=k) proportional to k**-0.99. A separate seeded
# permutation prevents key order from fixing where the hottest ranks live.
if 'zipfian' in a.scenes:
    zipf_cdf=list(itertools.accumulate(k**-0.99 for k in range(1,n+1)))
    zipf_keys=list(range(n))
    random.Random(183).shuffle(zipf_keys)
for name,n,ops,reads,hot in [('uniform',n,ops,0,False),('hot',n,ops,0,True),('lookup90',n,ops,.9,False),('long',n,5*ops,0,False),('zipfian',n,ops,0,False)]:
    if name not in a.scenes: continue
    with (a.output/('radix_'+name+'.data')).open('wb') as f:
        f.write(struct.pack('QQ',n,ops))
        for i in range(n): f.write(struct.pack('Q',i))
        for i in range(ops):
            if name == 'zipfian':
                rank=min(n-1,bisect.bisect_right(zipf_cdf,r.random()*zipf_cdf[-1]))
                key=zipf_keys[rank]
            else:
                key=r.randrange(min(100,n) if hot and r.random()<.9 else n)
            read=r.random()<reads
            f.write(struct.pack('B7xQQ',0 if read else 1,key,0 if read else i))
