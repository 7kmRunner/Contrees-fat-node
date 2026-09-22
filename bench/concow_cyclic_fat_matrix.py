"""Original cyclic pipeline, slots 0/2/4/8; no independent batch controller."""
import csv
import json
import subprocess
from pathlib import Path

output=Path('bench/results/concow_cyclic_fat_arm64.csv')
with output.open('w',newline='') as out:
    writer=None
    for run in range(4):
        for paced in (False,True):
            workers_list=(1,) if paced else (1,2,4)
            for pinned in (False,True):
                for workers in workers_list:
                    slots_order=(0,2,8,4) if run%2==0 else (4,8,2,0)
                    for slots in slots_order:
                        cmd=['bench/concow_cyclic_fat_bench','--records','10000' if paced else '100000',
                             '--updates','3000' if paced else '100000','--clients','1' if paced else '4',
                             '--workers',str(workers),'--slots',str(slots)]
                        if paced:cmd+=['--wait-each']
                        if pinned:cmd+=['--pinned']
                        result=subprocess.run(cmd,text=True,capture_output=True,timeout=60)
                        if result.returncode:raise RuntimeError(f'{cmd}: {result.returncode} {result.stderr}')
                        row=json.loads(result.stdout)
                        assert row['native_pipeline'] and row['reference_ok']
                        assert row['pending_bytes']==0 and row['retired_bytes']==row['reclaimed_bytes']
                        assert row['appended_updates']+row['cow_updates']==row['updates']
                        if run:
                            row={'run':run,**row}
                            if writer is None:
                                writer=csv.DictWriter(out,fieldnames=row.keys());writer.writeheader()
                            writer.writerow(row);out.flush()
                    print(f'run={run} paced={paced} pinned={pinned} workers={workers}',flush=True)
