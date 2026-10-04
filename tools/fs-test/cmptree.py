#!/usr/bin/env python3
"""cmptree.py HARNESS IMG SRCDIR -> compares every file in SRCDIR with what the driver reads (FNV-1a 64)."""
import os, subprocess, sys
harness, img, src = sys.argv[1:4]
def fnv(b):
    h = 1469598103934665603
    for x in b:
        h ^= x; h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
files = []
for root, _, fs in os.walk(src):
    for f in fs:
        p = os.path.join(root, f)
        files.append(os.path.relpath(p, src))
cmds = ['hash:' + f for f in files]
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
out = subprocess.run([harness, img, 'script'] + cmds, capture_output=True, text=True, env=env).stdout
got = {}
for line in out.splitlines():
    if line.startswith('HASH '):
        _, name, n, h = line.split(' ')[0], line[5:].rsplit(' ', 2)[0], *line.rsplit(' ', 2)[1:]
        got[line[5:].rsplit(' ', 2)[0]] = (int(n), int(h, 16))
bad = 0
for f in files:
    d = open(os.path.join(src, f), 'rb').read()
    exp = (len(d), fnv(d))
    if got.get(f) != exp:
        bad += 1
        print('MISMATCH', f, got.get(f), exp)
print('%d files compared, %d mismatches' % (len(files), bad))
sys.exit(1 if bad else 0)
