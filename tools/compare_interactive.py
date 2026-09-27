#!/usr/bin/env python3
"""Measure two real TUI sessions for the same wall time on a pseudo-terminal."""
import argparse
import fcntl
import json
import os
import pathlib
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time

root = pathlib.Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--seconds', type=float, default=10)
p.add_argument('--interval', type=float, default=1)
p.add_argument('--spawn', type=int, default=0)
p.add_argument('--repeat', type=int, default=1)
p.add_argument('--apps', nargs='+', choices=['btitop','top','htop'], default=['btitop','top'])
p.add_argument('--output', type=pathlib.Path)
args = p.parse_args()
workload = []
results = []

def run(name):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 110, 0, 0))
    env = dict(os.environ, TERM='xterm-256color', XDG_CONFIG_HOME='/nonexistent/btitop-benchmark-config', LC_ALL='C')
    app = ([str(root/'build/btitop'), '--backend=bpf', '--interval', str(args.interval)]
           if name == 'btitop' else
           ['top', '-d', str(args.interval)] if name == 'top' else
           ['htop', '-d', str(max(1, int(args.interval*10)))])
    with tempfile.TemporaryDirectory() as tmp:
        metric = pathlib.Path(tmp)/'time.txt'
        cmd = ['/usr/bin/time', '-f', '%U %S %M %e', '-o', str(metric), *app]
        def tty_setup():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, env=env,
                                preexec_fn=tty_setup, close_fds=True)
        os.close(slave)
        data_bytes = 0
        started = time.monotonic()
        deadline = started + args.seconds
        try:
            while time.monotonic() < deadline and proc.poll() is None:
                ready, _, _ = select.select([master], [], [], 0.1)
                if ready:
                    try: data_bytes += len(os.read(master, 65536))
                    except OSError: break
            os.write(master, b'q')
            proc.wait(timeout=3)
            while True:
                ready, _, _ = select.select([master], [], [], 0.02)
                if not ready: break
                try: data_bytes += len(os.read(master, 65536))
                except OSError: break
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=3)
            os.close(master)
        if proc.returncode:
            raise RuntimeError(f'{name} exited {proc.returncode}')
        user, system, peak_rss, elapsed = map(float, metric.read_text().split())
        return {'app': name, 'seconds_requested': args.seconds, 'wall_seconds': elapsed,
                'cpu_seconds': round(user+system,3), 'cpu_percent_one_core': round(100*(user+system)/elapsed,2),
                'user_seconds': user, 'system_seconds': system, 'peak_rss_kib': int(peak_rss),
                'terminal_bytes': data_bytes}

try:
    for _ in range(args.spawn):
        workload.append(subprocess.Popen(['sleep','120'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    for repeat in range(args.repeat):
        for name in (args.apps if repeat%2 == 0 else list(reversed(args.apps))):
            value = run(name)
            value['extra_tasks'] = args.spawn
            value['repeat'] = repeat
            results.append(value)
            print(json.dumps(value), flush=True)
finally:
    for child in workload:
        child.terminate()
    for child in workload:
        child.wait()
if args.output:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2)+'\n')
