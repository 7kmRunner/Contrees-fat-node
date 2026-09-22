"""Compare instrumented and uninstrumented runs; execute children sequentially."""
import argparse
import csv
import json
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, default=Path('bench/results/concow_fat_profile_arm64.csv'))
p.add_argument('--runs', type=int, default=3)
p.add_argument('--tune', action='store_true', help='uninstrumented dispatch threshold sweep')
a = p.parse_args()
if a.runs < 1:
    p.error('runs must be positive')
with a.output.open('w', newline='') as out:
    writer = None
    for run in range(a.runs + 1):
        for pattern in ('uniform', 'hot', 'zipfian'):
            configs = [(w, profile, 16) for w in (1, 2, 4) for profile in (0, 1)]
            if a.tune:
                configs = [(1, 0, 16)] + [(w, 0, threshold) for w in (2, 4)
                    for threshold in (16, 64, 128, 256, 1048576)]
            if run % 2:
                configs.reverse()
            for workers, profile, threshold in configs:
                binary = 'bench/concow_fat_profile' if profile else 'bench/concow_fat_bench'
                cmd = [binary, '--records', '100000', '--updates', '300000', '--clients', '4',
                       '--slots', '2', '--workers', str(workers), '--pattern', pattern,
                       '--parallel-materialization', '--parallel-min-groups', str(threshold)]
                result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
                if result.returncode:
                    raise RuntimeError(f'{cmd}: {result.returncode} {result.stderr}')
                row = json.loads(result.stdout)
                assert row['reference_ok'] and row['pending_bytes'] == 0
                assert row['retired_bytes'] == row['reclaimed_bytes']
                assert row['appended_updates'] + row['cow_updates'] == row['updates']
                if not profile:
                    assert row['plan_ns'] == row['worker_loop_ns'] == 0
                row = {'run': run, **row}
                if run:
                    if writer is None:
                        writer = csv.DictWriter(out, fieldnames=row.keys())
                        writer.writeheader()
                    writer.writerow(row)
                    out.flush()
            print(f'round={run} pattern={pattern}', flush=True)
