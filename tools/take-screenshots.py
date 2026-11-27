#!/usr/bin/env python3
"""Boots tOS in QEMU, drives the desktop and takes a screenshot of each new feature, for the docs.

    take-screenshots.py KERNEL INITRD OUTDIR

Writes OUTDIR/<name>.png (1280x720). Uses tools/qemu_drive.py to move the pointer, click and type.
.github/workflows/screenshots.yml runs it. Only for tests and docs of the project's own builds.
"""
import os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_drive

kernel, initrd, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
work = tempfile.mkdtemp(prefix='tos-shots-')
sock, dbg, ser = work + '/q.sock', work + '/dbg.log', work + '/serial.txt'
accel = ['-accel', 'kvm'] if os.environ.get('TOS_KVM') and os.access('/dev/kvm', os.W_OK) else []
qemu = subprocess.Popen(['qemu-system-x86_64', '-kernel', kernel, '-initrd', initrd, '-m', '4096', '-vga', 'std', '-display', 'none',
                         '-monitor', 'unix:%s,server,nowait' % sock, '-serial', 'file:' + ser, '-debugcon', 'file:' + dbg] + accel,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.1)
open(dbg, 'a').close()
d = qemu_drive.Drive(sock, dbg)
n = [0]


def shot(name):
    ppm = '%s/%s.ppm' % (work, name)
    d.hmp('screendump ' + ppm, 0.2)
    end = time.time() + 3
    last = -1
    while time.time() < end:
        sz = os.path.getsize(ppm) if os.path.exists(ppm) else 0
        if sz > 100000 and sz == last:
            break
        last = sz
        time.sleep(0.05)
    png = '%s/%s.png' % (out, name)
    subprocess.check_call(['convert', ppm, png])
    n[0] += 1
    print(time.strftime('%T'), name, flush=True)


def click(x, y):
    d.move(x, y)
    d.click()
    time.sleep(1.5)


DOCK = {'files': 343, 'terminal': 401, 'notepad': 517, 'calc': 575, 'settings': 865}

if not d.login(ser):
    print('login failed', flush=True)
    qemu.kill()
    sys.exit(1)
time.sleep(4)
shot('desktop')

# themes in Settings
click(DOCK['settings'], 678)
time.sleep(2)
shot('settings_appearance')

# a few windows, then Mission Control
for x in (517, 575, 343):
    click(x, 678)
d.key('ctrl-up')
time.sleep(4)
shot('mission_control')
d.key('esc')
time.sleep(2)

# search
d.hmp('sendkey ctrl-spc 300', 0.3)
time.sleep(2)
d.type('calc')
time.sleep(2)
shot('search')
d.key('esc')
time.sleep(1)

# Alt+Tab
d.hmp('sendkey alt-tab 4000', 0.3)
time.sleep(1.5)
shot('alt_tab')
time.sleep(3)

qemu.kill()
print(n[0], 'screenshots in', out, flush=True)