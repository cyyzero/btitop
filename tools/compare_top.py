#!/usr/bin/env python3
"""Compare btitop backends with procps top under temporary workloads.

Run with BPF privileges, e.g. sudo python3 tools/compare_top.py --output /tmp/compare.json.
All workload children are stopped in a finally block.
"""
import argparse
import json
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=pathlib.Path, default=ROOT / 'build/btitop')
parser.add_argument('--iterations', type=int, default=15)
parser.add_argument('--interval', type=float, default=0.1)
parser.add_argument('--output', type=pathlib.Path)
parser.add_argument('--cases', nargs='+', default=['baseline', 'sleep200', 'sleep500', 'threads128', 'threads128-H', 'busy4', 'churn'])
args = parser.parse_args()
if args.iterations < 3 or args.interval < 0.1:
    parser.error('at least three iterations and 0.1s interval required')
args.binary = args.binary.resolve()
if not args.binary.exists():
    parser.error(f'binary not found: {args.binary}')

class Workload:
    def __init__(self, case):
        self.case = case
        self.children = []
        self.stop = threading.Event()
        self.thread = None
    def __enter__(self):
        if self.case.startswith('sleep'):
            for _ in range(int(self.case.removeprefix('sleep'))):
                self.children.append(subprocess.Popen(['sleep', '120'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        elif self.case.startswith('threads'):
            count = int(self.case.removeprefix('threads').split('-')[0])
            code = 'import threading,time,sys\nthreads=[threading.Thread(target=time.sleep,args=(120,),daemon=True) for _ in range(int(sys.argv[1]))]\n[t.start() for t in threads]\ntime.sleep(120)'
            self.children.append(subprocess.Popen([sys.executable, '-c', code, str(count)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        elif self.case.startswith('busy'):
            for _ in range(int(self.case.removeprefix('busy'))):
                self.children.append(subprocess.Popen(['yes'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        elif self.case == 'churn':
            def churn():
                while not self.stop.is_set():
                    proc = subprocess.Popen(['sleep', '0.02'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    proc.wait()
            self.thread = threading.Thread(target=churn, daemon=True)
            self.thread.start()
        elif self.case != 'baseline':
            raise ValueError(f'unknown case: {self.case}')
        time.sleep(0.25)
        return self
    def __exit__(self, *_):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=2)
        for child in self.children:
            if child.poll() is None:
                child.terminate()
        for child in self.children:
            child.wait()

def command(name, case, json_mode=False):
    thread_mode = case.endswith('-H')
    if name == 'top':
        return ['top', '-b', '-n', str(args.iterations), '-d', str(args.interval), '-w', '120'] + (['-H'] if thread_mode else [])
    cmd = [str(args.binary), '--backend', name, '--iterations', str(args.iterations), '--interval', str(args.interval)]
    cmd += ['--json' if json_mode else '--batch']
    if thread_mode:
        cmd.append('--threads')
    return cmd

def timed(cmd, env):
    with tempfile.TemporaryDirectory() as tmp:
        metric = pathlib.Path(tmp) / 'metric'
        result = subprocess.run(['/usr/bin/time', '-f', '%U %S %M %e', '-o', str(metric), *cmd],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, env=env, timeout=120)
        if result.returncode:
            raise RuntimeError(f'{cmd[0]} exited {result.returncode}: {result.stderr[-500:]}')
        user, system, rss, wall = map(float, metric.read_text().split())
        return {'cpu_seconds': round(user + system, 4), 'user_seconds': user,
                'system_seconds': system, 'peak_rss_kib': int(rss), 'wall_seconds': wall}

def latency(cmd, env):
    result = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr[-500:])
    frames = [json.loads(line) for line in result.stdout.splitlines()]
    ms = sorted((x['end_monotonic_ns'] - x['begin_monotonic_ns']) / 1e6 for x in frames)
    return {'tasks_median': int(statistics.median(len(x['tasks']) for x in frames)),
            'sample_ms_p50': round(statistics.median(ms), 3),
            'sample_ms_p95': round(ms[min(len(ms) - 1, int((len(ms) - 1) * .95))], 3)}

results = []
env = dict(os.environ, XDG_CONFIG_HOME='/nonexistent/btitop-benchmark-config', LC_ALL='C')
for case in args.cases:
    print(f'CASE {case}', flush=True)
    with Workload(case):
        runs = {}
        for name in ('bpf', 'procfs', 'top'):
            runs[name] = timed(command(name, case), env)
            if name != 'top':
                runs[name].update(latency(command(name, case, True), env))
            print(name, json.dumps(runs[name]), flush=True)
        results.append({'case': case, 'runs': runs})
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'kernel': os.uname().release, 'iterations': args.iterations,
                                       'interval_seconds': args.interval, 'results': results}, indent=2) + '\n')
