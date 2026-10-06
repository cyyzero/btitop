#!/usr/bin/env python3
"""Check the public JSON interface against a live, stable process."""
import json
import os
import subprocess
import sys

binary = sys.argv[1]
probe = subprocess.Popen(['sleep', '5'])
try:
    result = subprocess.run(
        [binary, '--backend=procfs', '--json', '--pid', str(probe.pid),
         '--iterations=2', '--interval=0.1'],
        text=True, capture_output=True, check=True, timeout=4)
    frames = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(frames) == 2
    assert frames[0]['version'] == 1
    assert frames[0]['backend'] == 'procfs'
    assert frames[0]['mode'] == 'top'
    assert frames[0]['tasks'][0]['pid'] == probe.pid
    assert frames[0]['tasks'][0]['cpu_percent'] is None
    assert frames[1]['tasks'][0]['cpu_percent'] is not None
    assert frames[1]['tasks'][0]['start_ns'] == frames[0]['tasks'][0]['start_ns']
    assert frames[1]['system']['cpu_percent'] is not None
finally:
    probe.terminate()
    probe.wait()

# Keep several threads alive while comparing process and thread views. This
# verifies that the htop-style backend traverses tasks even with threads hidden.
threaded = subprocess.Popen(
    [sys.executable, '-u', '-c',
     'import threading, time\n'
     'for _ in range(3):\n'
     '    threading.Thread(target=time.sleep, args=(10,), daemon=True).start()\n'
     'print("ready", flush=True)\n'
     'time.sleep(10)\n'],
    stdout=subprocess.PIPE, text=True)
try:
    assert threaded.stdout.readline().strip() == 'ready'
    def snapshot(*extra):
        result = subprocess.run(
            [binary, '--mode=htop', '--backend=procfs', '--json', '--pid', str(threaded.pid),
             '--iterations=1', *extra],
            text=True, capture_output=True, check=True, timeout=4)
        return json.loads(result.stdout)

    process = snapshot()
    thread = snapshot('--threads')
    assert process['backend'] == thread['backend'] == 'procfs'
    assert process['mode'] == thread['mode'] == 'htop'
    assert len(process['tasks']) == 1
    assert len(thread['tasks']) >= 4
    assert process['scanned_tasks'] >= len(thread['tasks'])
    assert thread['scanned_tasks'] >= len(thread['tasks'])
    leader = next(t for t in thread['tasks'] if t['tid'] == threaded.pid)
    assert all(t['pid'] == threaded.pid for t in thread['tasks'])
    assert all(t['rss_bytes'] == leader['rss_bytes'] for t in thread['tasks'])
    assert all(t['shared_bytes'] == leader['shared_bytes'] for t in thread['tasks'])
    alias = subprocess.run([binary, '--backend=htop', '--json', '--iterations=1', '--pid', str(threaded.pid)],
                           text=True, capture_output=True, check=True, timeout=4)
    assert json.loads(alias.stdout)['mode'] == 'htop'
    forced_fallback = subprocess.run(
        [binary, '--mode=htop', '--backend=auto', '--json', '--iterations=1',
         '--pid', str(threaded.pid)],
        env={**os.environ, 'BTITOP_BPF_OBJECT': '/nonexistent/btitop-test.o'},
        text=True, capture_output=True, check=True, timeout=4)
    fallback = json.loads(forced_fallback.stdout)
    assert fallback['mode'] == 'htop' and fallback['backend'] == 'procfs'
    assert fallback['scanned_tasks'] >= len(thread['tasks'])
finally:
    threaded.terminate()
    threaded.wait()
