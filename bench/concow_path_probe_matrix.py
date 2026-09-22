"""Same native cyclic scheduler: slots off/on and global/local probe gates.

Runs fresh processes sequentially, randomizes configuration order, and checks
the reference model and GC drain before retaining a measurement.
"""
import argparse
import csv
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--binary', default='bench/concow_cyclic_fat_bench')
    parser.add_argument('--output', default='bench/results/concow_path_probe_arm64.csv')
    parser.add_argument('--records', type=int, default=100000)
    parser.add_argument('--updates', type=int, default=300000)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--warmups', type=int, default=1)
    args = parser.parse_args()
    if min(args.records, args.updates, args.runs) < 1 or args.warmups < 0:
        parser.error('positive records/updates/runs and nonnegative warmups required')
    binary = Path(args.binary).resolve()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    metadata_path = output.with_suffix('.metadata.json')
    configs = list(itertools.product((1, 2, 4), ('uniform', 'hot'), (False, True),
                                   ((0, True), (2, False), (2, True),
                                    (4, False), (4, True), (8, False), (8, True))))
    metadata = {
        'arguments': vars(args), 'platform': platform.platform(),
        'cpu_count': os.cpu_count(),
        'compiler': subprocess.check_output(['clang++', '--version'], text=True),
        'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'source_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(set(Path('lib').rglob('*.hpp')) |
                                          set(Path('src').rglob('*.hpp')) |
                                          {Path('bench/concow_fat_bench.cpp')})},
        'seed': 20260916, 'sequential_processes': True, 'commands': [],
        'completed': False,
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
    rng = random.Random(metadata['seed'])
    started = time.monotonic()
    with output.open('w', newline='') as out:
        writer = None
        for run in range(-args.warmups + 1, args.runs + 1):
            order = configs.copy()
            rng.shuffle(order)
            for index, (workers, pattern, pinned, (slots, local)) in enumerate(order):
                command = [str(binary), '--records', str(args.records), '--updates', str(args.updates),
                           '--clients', '4', '--workers', str(workers), '--slots', str(slots),
                           '--pattern', pattern]
                if pinned:
                    command.append('--pinned')
                if not local:
                    command.append('--global-probe')
                result = subprocess.run(command, capture_output=True, text=True, timeout=120, check=True)
                row = json.loads(result.stdout)
                if not (row['reference_ok'] and row['native_pipeline'] and
                        row['pending_bytes'] == 0 and row['retired_bytes'] == row['reclaimed_bytes'] and
                        row['appended_updates'] + row['cow_updates'] == args.updates):
                    raise RuntimeError(f'invalid sample: {row}')
                metadata['commands'].append({'run': run, 'command': command})
                if run > 0:
                    row = {'run': run, 'order': index, **row}
                    if writer is None:
                        writer = csv.DictWriter(out, fieldnames=row)
                        writer.writeheader()
                    writer.writerow(row)
                    out.flush()
                if (index + 1) % 21 == 0:
                    print(f'run={run} completed={index+1}/{len(order)} elapsed={time.monotonic()-started:.1f}s', flush=True)
            metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')
    metadata['completed'] = True
    metadata['elapsed_seconds'] = time.monotonic() - started
    metadata_path.write_text(json.dumps(metadata, indent=2) + '\n')


if __name__ == '__main__':
    main()
