#!/usr/bin/env python3
"""Compare the top/htop scan modes across BPF and procfs backends.

Run as root so the explicit BPF backend can load. Workloads are stopped even
if a run fails. The JSON output retains every repetition for later analysis.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import resource
import select
import statistics
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--binary', type=Path, default=ROOT / 'build/btitop')
p.add_argument('--scenarios', default='0:0,100:0,300:0,0:128,0:512,100:128',
               help='comma-separated extra-sleep-processes:threads-in-one-extra-worker-process pairs')
p.add_argument('--rounds', type=int, default=3)
p.add_argument('--iterations', type=int, default=6)
p.add_argument('--interval', type=float, default=0.2)
p.add_argument('--output', type=Path)
p.add_argument('--references-only', action='store_true',
               help='measure actual top and htop TUI programs on a 110x30 pseudo-terminal')
p.add_argument('--reference-apps', nargs='+', choices=['top', 'htop', 'htop-hidden'],
               default=['top', 'htop'], help='reference programs; htop-hidden hides user threads')
p.add_argument('--tui-matrix-only', action='store_true',
               help='measure four btitop mode/backend combinations on a 110x30 pseudo-terminal')
p.add_argument('--reference-seconds', type=float, default=2)
args = p.parse_args()
if args.rounds < 1 or args.iterations < 3 or not 0.1 <= args.interval <= 3600:
    p.error('rounds >= 1, iterations >= 3 and interval >= 0.1 are required')
if args.references_only and args.tui_matrix_only:
    p.error('choose one terminal benchmark type')
try:
    scenarios = [tuple(map(int, item.split(':'))) for item in args.scenarios.split(',')]
    if not scenarios or any(len(x) != 2 or min(x) < 0 for x in scenarios):
        raise ValueError()
except ValueError:
    p.error('scenarios must be nonnegative extra-sleep-process:worker-thread pairs')
binary = args.binary.resolve()
if not binary.is_file():
    p.error(f'binary not found: {binary}')

def run(mode, backend):
    cmd = [str(binary), '--mode', mode, '--backend', backend, '--json',
           '--iterations', str(args.iterations), '--interval', str(args.interval)]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    result = subprocess.run(cmd, text=True, capture_output=True,
                            timeout=max(30, args.iterations * args.interval * 3))
    wall = time.monotonic() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if result.returncode:
        raise RuntimeError(f'{mode}/{backend}: {result.stderr.strip()}')
    frames = [json.loads(line) for line in result.stdout.splitlines()]
    if len(frames) != args.iterations or any(f['mode'] != mode or f['backend'] != backend for f in frames):
        raise RuntimeError(f'{mode}/{backend}: unexpected frames or backend fallback')
    latencies = sorted((f['end_monotonic_ns'] - f['begin_monotonic_ns']) / 1e6 for f in frames)
    cpu = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
    return {'mode': mode, 'backend': backend, 'cpu_seconds': cpu,
            'cpu_percent_one_core': 100 * cpu / wall, 'wall_seconds': wall,
            'sample_ms_p50': statistics.median(latencies),
            'sample_ms_p95': latencies[min(len(latencies) - 1, int(len(latencies) * .95))],
            'rows_median': statistics.median(len(f['tasks']) for f in frames),
            'scanned_median': statistics.median(f['scanned_tasks'] for f in frames)}

def run_terminal(config):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 110, 0, 0))
    if isinstance(config, tuple):
        mode, backend = config
        cmd = [str(binary), '--mode', mode, '--backend', backend,
               '--interval', str(args.interval)]
        label = f'{mode}/{backend}'
    else:
        cmd = (['top', '-d', str(args.interval)] if config == 'top' else
               ['htop', '-d', str(max(1, round(args.interval * 10)))])
        label = config
    with tempfile.TemporaryDirectory() as config_dir:
        env = dict(os.environ, TERM='xterm-256color', LC_ALL='C', XDG_CONFIG_HOME=config_dir)
        if config == 'htop-hidden':
            htoprc = Path(config_dir) / 'htoprc'
            htoprc.write_text('htop_version=3.3.0\nhide_userland_threads=1\n')
            env['HTOPRC'] = str(htoprc)
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.monotonic()
        def tty_setup():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        child = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave,
                                 env=env, preexec_fn=tty_setup)
        os.close(slave)
        terminal_bytes = 0
        try:
            while time.monotonic() - start < args.reference_seconds and child.poll() is None:
                if select.select([master], [], [], .1)[0]:
                    try:
                        terminal_bytes += len(os.read(master, 65536))
                    except OSError:
                        break
            if child.poll() is None:
                os.write(master, b'q')
            child.wait(timeout=3)
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=3)
            os.close(master)
        wall = time.monotonic() - start
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        if child.returncode:
            raise RuntimeError(f'{label} exited with {child.returncode}')
        cpu = after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime
        result = {'cpu_seconds': cpu, 'cpu_percent_one_core': 100 * cpu / wall,
                  'wall_seconds': wall, 'terminal_bytes': terminal_bytes}
        if isinstance(config, tuple):
            result.update(mode=mode, backend=backend)
        else:
            result['reference_app'] = config
        return result

def start_workload(processes, threads):
    children = []
    try:
        for _ in range(processes):
            children.append(subprocess.Popen(['sleep', '120'], stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL))
        if threads:
            code = ('import sys, threading, time\n'
                    'workers = [threading.Thread(target=time.sleep, args=(120,), daemon=True) '
                    'for _ in range(int(sys.argv[1]))]\n'
                    'for worker in workers: worker.start()\n'
                    'print("ready", flush=True)\n'
                    'time.sleep(120)\n')
            child = subprocess.Popen([sys.executable, '-u', '-c', code, str(threads)],
                                     stdout=subprocess.PIPE, text=True)
            children.append(child)
            if child.stdout.readline().strip() != 'ready':
                raise RuntimeError('thread workload failed to start')
        return children
    except BaseException:
        stop_workload(children)
        raise

def stop_workload(children):
    for child in children:
        if child.poll() is None:
            child.terminate()
    for child in children:
        child.wait()

results = []
configs = [('top', 'bpf'), ('top', 'procfs'), ('htop', 'bpf'), ('htop', 'procfs')]
for processes, threads in scenarios:
    children = start_workload(processes, threads)
    try:
        for round_number in range(args.rounds):
            if args.references_only:
                ordered = args.reference_apps if round_number % 2 == 0 else list(reversed(args.reference_apps))
            elif args.tui_matrix_only:
                ordered = configs[round_number % 4:] + configs[:round_number % 4]
            else:
                # Rotate order so a consistently warmer or busier host does not
                # always favor the same backend.
                ordered = configs[round_number % 4:] + configs[:round_number % 4]
            for config in ordered:
                if args.references_only or args.tui_matrix_only:
                    value = run_terminal(config)
                else:
                    mode, backend = config
                    value = run(mode, backend)
                value.update(extra_processes=processes, extra_threads=threads,
                             added_processes_total=processes + (threads > 0),
                             added_tasks_total=processes + (threads + 1 if threads else 0),
                             round=round_number + 1)
                results.append(value)
                print(json.dumps(value), flush=True)
    finally:
        stop_workload(children)

document = {'kernel': os.uname().release, 'interval_seconds': args.interval,
            'iterations': args.iterations, 'rounds': args.rounds,
            'terminal_seconds': args.reference_seconds if args.references_only or args.tui_matrix_only else None,
            'results': results}
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + '\n')
