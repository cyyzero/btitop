#!/usr/bin/env python3
"""Capture a real btitop terminal session as a PNG."""
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path

import pyte
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
master, slave = pty.openpty()
cols, rows = 110, 30
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
env = dict(os.environ, TERM='xterm-256color')
proc = subprocess.Popen([str(root/'build/btitop'), '--backend=procfs', '--interval=0.3'],
                        stdin=slave, stdout=slave, stderr=slave, env=env)
os.close(slave)
screen = pyte.Screen(cols, rows)
stream = pyte.Stream(screen)
end = time.monotonic() + 1.5
while time.monotonic() < end:
    ready, _, _ = select.select([master], [], [], 0.1)
    if ready:
        try: stream.feed(os.read(master, 65536).decode('utf-8', 'replace'))
        except OSError: break
quiet_end = time.monotonic() + 0.1
while time.monotonic() < quiet_end:
    ready, _, _ = select.select([master], [], [], 0.03)
    if ready:
        try: stream.feed(os.read(master, 65536).decode('utf-8', 'replace'))
        except OSError: break
        quiet_end = time.monotonic() + 0.1
lines = list(screen.display)
os.write(master, b'q')
proc.wait(timeout=3)
os.close(master)
font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 17)
width, height = 11, 23
img = Image.new('RGB', (cols*width+32, rows*height+32), '#10151e')
draw = ImageDraw.Draw(img)
for i, line in enumerate(lines):
    draw.text((16, 16+i*height), line.rstrip(), font=font, fill='#dce6f2')
output = root/'assets/screenshot.png'
output.parent.mkdir(exist_ok=True)
img.save(output)
print(output)
