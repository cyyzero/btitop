#!/usr/bin/env python3
"""Repeatable collection benchmark. Run as root to compare both backends."""
import argparse
import json
import resource
import statistics
import subprocess
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, default=Path('build/btitop'))
parser.add_argument('--iterations', type=int, default=20)
parser.add_argument('--interval', type=float, default=0.1)
parser.add_argument('--spawn', type=int, default=0, help='extra sleeping tasks; limited by host process limits')
args = parser.parse_args()
if args.iterations < 2 or args.spawn < 0:
    parser.error('need at least two iterations and nonnegative spawn count')
children = []
try:
    for _ in range(args.spawn):
        children.append(subprocess.Popen(['sleep', '120'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    for backend in ('bpf', 'procfs'):
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.monotonic()
        cmd = [str(args.binary.resolve()), '--backend', backend, '--json', '--iterations', str(args.iterations), '--interval', str(args.interval)]
        result = subprocess.run(cmd, text=True, capture_output=True)
        elapsed = time.monotonic() - start
        if result.returncode:
            print(backend, 'FAILED:', result.stderr.strip())
            continue
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        samples = [json.loads(line) for line in result.stdout.splitlines()]
        latency = sorted((x['end_monotonic_ns']-x['begin_monotonic_ns'])/1e6 for x in samples)
        def quantile(p): return latency[min(len(latency)-1, int((len(latency)-1)*p))]
        print(json.dumps({
            'backend': backend, 'samples': len(samples), 'tasks_last': len(samples[-1]['tasks']),
            'wall_seconds': round(elapsed, 3),
            'cpu_seconds': round(after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime, 3),
            'sample_ms_p50': round(quantile(.5), 3), 'sample_ms_p95': round(quantile(.95), 3),
            'maxrss_kib_observed': after.ru_maxrss,
        }))
finally:
    for child in children:
        child.terminate()
    for child in children:
        child.wait()
