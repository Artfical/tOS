#!/usr/bin/env python3
"""Records tOS's animations: boots it in QEMU, drives the desktop, and makes a video of the screen.

    record-animations.py KERNEL INITRD OUT.mp4 [SLOW]

KERNEL is built with ANIM_SLOW=SLOW (default 6; the workflow uses 12): the animations then run that many times slower, QEMU's
screendump can follow them, and every frame is shown for 1/SLOW of the time it took, so they play at their
real speed. Waiting and the pointer's travel are sped up. Needs qemu-system-x86_64, ffmpeg, the fonts of DejaVu.
.github/workflows/animations.yml runs it on every change of the desktop.
"""
import os, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_drive

kernel, initrd, out = sys.argv[1:4]
SLOW = float(sys.argv[4]) if len(sys.argv) > 4 else 6.0
work = tempfile.mkdtemp(prefix='tos-rec-')
sock, dbg, ser = work + '/q.sock', work + '/dbg.log', work + '/serial.txt'
accel = ['-accel', 'kvm'] if os.environ.get('TOS_KVM') and os.access('/dev/kvm', os.W_OK) else []
qemu = subprocess.Popen(['qemu-system-x86_64', '-kernel', kernel, '-initrd', initrd, '-m', '4096', '-vga', 'std', '-display', 'none',
                         '-monitor', 'unix:%s,server,nowait' % sock, '-serial', 'file:' + ser, '-debugcon', 'file:' + dbg, '-no-reboot', '-d', 'cpu_reset,guest_errors', '-D', work + '/qemu-debug.log'] + accel,
                        stdout=subprocess.DEVNULL, stderr=open(work + '/qemu.log', 'w'))
for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
open(dbg, 'a').close()
d = qemu_drive.Drive(sock, dbg)

frames = []            # (time, file, speed)
marks = []             # (frame index, caption)
speed = [10.0]
ext = ['png']


def grab():
    f = '%s/f%06d.%s' % (work, len(frames), ext[0])
    d.s.send(('screendump %s%s\n' % (f, ' -f png' if ext[0] == 'png' else '')).encode())
    t = time.time()
    end = t + 3
    last = -1
    while time.time() < end:                    # (the file is done when it stops growing)
        sz = os.path.getsize(f) if os.path.exists(f) else 0
        if sz > 100 and sz == last:
            break
        last = sz
        time.sleep(0.04)
    d._read()
    if not os.path.exists(f):
        if ext[0] == 'png':                     # an older QEMU: no PNG screendumps
            ext[0] = 'ppm'
        return
    if ext[0] == 'ppm':                         # (smaller on disk)
        from PIL import Image
        j = f[:-3] + 'jpg'
        try:
            Image.open(f).save(j, quality=88)
        except OSError:
            os.remove(f)
            return
        os.remove(f)
        f = j
    frames.append((t, f, speed[0]))


d.idle = grab


def scene(text, sp=1.0):
    print(time.strftime('%T'), text, len(frames), 'frames', flush=True)
    marks.append((len(frames), text))
    speed[0] = sp


def hold(secs, sp=None):
    if sp:
        speed[0] = sp
    d.sleep(secs)


def to(x, y, sp=25.0):
    speed[0] = sp
    d.move(x, y)


def click_at(x, y, sp=25.0):
    to(x, y, sp)
    speed[0] = 1.0
    d.click()


# ---- the script: the positions are those of a 1280 x 720 desktop
DOCK = {'files': 343, 'terminal': 401, 'notepad': 517, 'calc': 575, 'settings': 865}

scene('tOS starts', 10.0)
d.wait_serial(ser, 'session: recovery', 900)
hold(2, 1.0)
scene('tOS Recovery', 1.0)
hold(3)
click_at(640, 425)
hold(1.5)
click_at(995, 640)
hold(2.5)
scene('Logging in', SLOW)
for _ in range(10):
    d.click()
    if d.wait_serial(ser, 'session: desktop', 20):
        break
hold(4 * SLOW / 3)
scene('The desktop', 1.0)
hold(3)
scene('The Dock magnifies', 2.0)
to(300, 685, 12.0)
to(1000, 685, 12.0)
to(640, 400, 12.0)
hold(1.5, 1.0)

if not os.environ.get('REC_ONLY_SPACES'):
    scene('Themes', 1.0)
    click_at(DOCK['settings'], 678)
    hold(4)
    for label, (x, y) in [('Classic: the old icons', (603, 175)), ('Midnight', (731, 175)), ('Ocean', (859, 175)),
                          ('Nord, a theme from a file', (473, 251)), ('Sunset, with its own icons', (591, 251)), ('tOS', (475, 175))]:
        scene(label, 1.0)
        click_at(x, y)
        hold(3.0, 1.0)
    scene('Close', 1.0)
    click_at(178, 45)
    hold(2)

    scene('Windows open', 1.0)
    click_at(DOCK['notepad'], 678)
    hold(3)
    scene('The genie', SLOW)
    click_at(228, 71)                       # the yellow button of the first window
    hold(0.5 * SLOW + 1.5, SLOW)
    scene('The genie, back out', SLOW)
    click_at(DOCK['notepad'], 678)
    hold(0.5 * SLOW + 1.5, SLOW)
click_at(DOCK['calc'], 678)
hold(3, 1.0)

scene('Mission Control', SLOW)
speed[0] = SLOW
d.key('ctrl-up', 0.3, 150)
hold(0.8 * SLOW + 2, SLOW)
scene('Spaces: add one', 1.0)
click_at(739, 93)
hold(2.5, 1.0)
scene('Drag a window to it', 1.0)
to(350, 400)
d.press()
hold(0.6, 1.0)
speed[0] = 12.0
d.glide(500, 250, 5)
d.glide(640, 100, 6)
hold(1.0, 1.0)
d.release()
hold(2.5, 1.0)
scene('Switch Space', SLOW)
click_at(640, 95)
hold(0.7 * SLOW + 2, SLOW)
hold(2, 1.0)
scene('Ctrl + arrows slide between Spaces', SLOW)
speed[0] = SLOW
d.key('ctrl-left', 0.3, 150)
hold(0.7 * SLOW + 2, SLOW)
hold(1.5, 1.0)
speed[0] = SLOW
d.key('ctrl-right', 0.3, 150)
hold(0.7 * SLOW + 2, SLOW)
hold(1.5, 1.0)
scene('Search: Ctrl + Space', 1.0)
speed[0] = 4.0
d.hmp('sendkey ctrl-spc 300', 0.3)
hold(2.0, 4.0)
d.type('calc')
hold(2.5, 1.0)
d.key('ret', 0.3, 150)
hold(3.0, 1.0)
scene('Alt + Tab: every window', 1.0)
speed[0] = 3.0
d.hmp('sendkey alt-tab 3500', 0.3)
hold(4.5, 3.0)
hold(2.0, 1.0)
scene('Drag to an edge: half the screen', 1.0)
to(300, 71)
d.press()
speed[0] = 6.0
d.glide(150, 200, 4)
d.glide(2, 300, 4)
hold(1.5, 2.0)
d.release()
hold(3.0, 4.0)
hold(1.5, 1.0)
scene('tOS', 1.0)
hold(3)
d.idle = None
if qemu.poll() is not None:
    print('QEMU ended early:', qemu.returncode, open(work + '/qemu.log').read()[-500:], open(work + '/qemu-debug.log').read()[-800:])
qemu.kill()

# ---- the video: every frame lasts as long as it did, divided by its speed; a run of identical frames (waiting) is cut to half a second
import hashlib
n = len(frames)
if not n:
    sys.exit('no frames')
digest = [hashlib.md5(open(f, 'rb').read()).digest() for _, f, _ in frames]
keep, dur_of = [], []
i = 0
while i < n:
    j = i
    total = 0.0
    while j < n and digest[j] == digest[i]:
        nxt = frames[j + 1][0] if j + 1 < n else frames[j][0] + 0.5
        total += max(0.001, (nxt - frames[j][0]) / frames[j][2])
        j += 1
    keep.append(i)
    dur_of.append(min(total, 0.5 if j - i > 1 else total))
    i = j
at, t_out, lst = {}, 0.0, []             # (at: when each captured frame is on screen, so the captions can find their place)
for kidx, idx in enumerate(keep):
    end = keep[kidx + 1] if kidx + 1 < len(keep) else n
    for f in range(idx, end):
        at[f] = t_out
    lst.append("file '%s'\nduration %.4f\n" % (frames[idx][1], dur_of[kidx]))
    t_out += dur_of[kidx]
lst.append("file '%s'\n" % frames[keep[-1]][1])
open(work + '/list.txt', 'w').write(''.join(lst))
draw = []
for k, (idx, text) in enumerate(marks):
    if idx >= n:
        continue
    t0 = at[idx]
    t1 = at[marks[k + 1][0]] if k + 1 < len(marks) and marks[k + 1][0] < n else t_out
    esc = text.replace("'", '').replace(':', '\\:')
    draw.append("drawtext=text='%s':fontfile=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf:fontsize=34:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=14:x=(w-text_w)/2:y=h-90:enable='between(t,%.3f,%.3f)'" % (esc, t0, t1))
vf = ','.join(['scale=1280:720', 'fps=30'] + draw + ['format=yuv420p'])
subprocess.check_call(['ffmpeg', '-v', 'error', '-y', '-f', 'concat', '-safe', '0', '-i', work + '/list.txt', '-vf', vf,
                       '-c:v', 'libx264', '-preset', 'medium', '-crf', '20', '-movflags', '+faststart', out])
print('%d frames, %.1f s' % (n, t_out))
shutil.rmtree(work, ignore_errors=True)
