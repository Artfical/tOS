#!/usr/bin/env python3
"""The boot screen's animation: an mp4 reduced to 640x360 baseline JPEG frames at 20 fps and
packed into assets/boot/bootvideo.bin (u32 count, u32 fps, u32 offsets[count + 1], then the frames),
which kernel/display/bootvideo.s links into the kernel. Run again after changing the video:
    tools/gen_bootvideo.py artfcal-logo-small.mp4"""
import os, struct, subprocess, sys, tempfile, glob
HERE = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1]
FPS = 20
with tempfile.TemporaryDirectory() as d:
    subprocess.check_call(['ffmpeg', '-v', 'error', '-i', src, '-vf', 'fps=%d,scale=640:360:flags=lanczos' % FPS,
                           '-q:v', '6', '-pix_fmt', 'yuvj420p', os.path.join(d, '%04d.jpg')])
    frames = [open(f, 'rb').read() for f in sorted(glob.glob(os.path.join(d, '*.jpg')))]
offs, o = [], 8 + 4 * (len(frames) + 1)
for f in frames:
    offs.append(o); o += len(f)
offs.append(o)
out = os.path.join(HERE, '..', 'assets', 'boot', 'bootvideo.bin')
with open(out, 'wb') as fh:
    fh.write(struct.pack('<II', len(frames), FPS))
    fh.write(struct.pack('<%dI' % len(offs), *offs))
    for f in frames: fh.write(f)
print(len(frames), 'frames,', o, 'bytes')
