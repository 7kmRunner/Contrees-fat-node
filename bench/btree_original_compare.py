"""Same driver, original git tree versus latest fat controller; Mac RSS bytes."""
import argparse
import csv
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--patched', action='store_true')
args = parser.parse_args()
baseline = 'patched' if args.patched else 'original'
output = Path(f'bench/results/btree_{baseline}_comparison.csv')
with output.open('w', newline='') as stream:
    writer = None
    for run in range(4):  # one warmup plus three measured runs
        for workload in ('update', 'insert'):
            configs = [(baseline, w) for w in (1, 2, 4)] + [('latest', 1)]
            if run % 2:
                configs.reverse()
            for mode, workers in configs:
                cmd = [f'build/btree_{mode}_compare', str(workers), workload, '100000', '300000']
                try:
                    result = subprocess.run(cmd, text=True, capture_output=True, timeout=45)
                    row = json.loads(result.stdout) if result.stdout.strip() else {}
                    row.update(exit_code=result.returncode, error=result.stderr[:500])
                except subprocess.TimeoutExpired:
                    row = {'exit_code': 'timeout', 'error': '45-second process timeout'}
                row.update(run=run, mode=mode, workload=workload, workers=workers)
                print(json.dumps(row), flush=True)
                if run:
                    if writer is None:
                        fields = ['run', 'mode', 'workload', 'workers', 'clients', 'gc', 'records',
                                  'updates', 'seconds', 'peak_rss_bytes', 'live_bytes', 'mismatches',
                                  'exit_code', 'error']
                        writer = csv.DictWriter(stream, fieldnames=fields)
                        writer.writeheader()
                    writer.writerow(row)
                    stream.flush()
