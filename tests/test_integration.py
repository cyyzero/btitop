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
    assert frames[0]['tasks'][0]['pid'] == probe.pid
    assert frames[0]['tasks'][0]['cpu_percent'] is None
    assert frames[1]['tasks'][0]['cpu_percent'] is not None
    assert frames[1]['tasks'][0]['start_ns'] == frames[0]['tasks'][0]['start_ns']
    assert frames[1]['system']['cpu_percent'] is not None
finally:
    probe.terminate()
    probe.wait()
