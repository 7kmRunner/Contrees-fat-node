"""Matched original ConCow versus ordered-wave BeTree, GC off in both."""
import csv
import json
import subprocess
from pathlib import Path

output = Path('bench/results/betree_original_current_comparison.csv')
with output.open('w', newline='') as stream:
    writer = None
    for run in range(4):
        for workload in ('update', 'insert'):
            configs = [(mode, w) for w in (1, 2, 4)
                       for mode in ('baseline', 'latest')]
            if run % 2:
                configs.reverse()
            for mode, workers in configs:
                command = [f'build/betree_{mode}_current', str(workers),
                           workload, '100000', '300000']
                result = subprocess.run(command, text=True, capture_output=True,
                                        timeout=60)
                if result.returncode:
                    raise RuntimeError(f'{command}: {result.returncode} '
                                       f'{result.stdout} {result.stderr}')
                row = {'run': run, 'mode': mode, **json.loads(result.stdout)}
                assert row['mismatches'] == 0
                if run:
                    if writer is None:
                        writer = csv.DictWriter(stream, fieldnames=row.keys())
                        writer.writeheader()
                    writer.writerow(row)
                    stream.flush()
            print(f'run={run} workload={workload}', flush=True)
