"""ART: shared workload, full reference check, fresh-process comparisons."""
import argparse
import csv
import json
import subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--records',type=int,default=100000)
p.add_argument('--updates',type=int,default=100000)
p.add_argument('--output',type=Path,default=Path('bench/results/concow_art_arm64.csv'))
a=p.parse_args()
if min(a.records,a.updates)<=0:p.error('positive sizes required')
with a.output.open('w',newline='') as out:
    writer=None
    for run in range(4):
        for pattern in ('uniform','hot','insert'):
            for pinned in (0,1):
                configs=[('cow',0,1),('seq',2,1),('batch',0,1),('batch',2,1),('batch',2,2),('batch',2,4),('batch',4,1),('batch',8,1)]
                if (run+pinned)%2:configs.reverse()
                for mode,slots,workers in configs:
                    cmd=['bench/concow_art_bench',mode,str(slots),str(workers),pattern,str(pinned),str(a.records),str(a.updates)]
                    result=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
                    if result.returncode:raise RuntimeError(f'{cmd}: {result.returncode} {result.stderr}')
                    row=json.loads(result.stdout)
                    assert row['reference_ok'] and row['pending_bytes']==0 and row['retired_bytes']==row['reclaimed_bytes']
                    if run:
                        row={'run':run,**row}
                        if writer is None:writer=csv.DictWriter(out,fieldnames=row.keys());writer.writeheader()
                        writer.writerow(row);out.flush()
                print(f'run={run} pattern={pattern} pinned={pinned}',flush=True)
