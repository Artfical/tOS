#!/usr/bin/env python3
"""Drives a QEMU-run tOS from outside: pointer, clicks, keys, screenshots.

    qemu-system-x86_64 ... -monitor unix:SOCK,server,nowait -debugcon file:DBG
    qemu_drive.py SOCK DBG move 640 360 | click | key ctrl-left | shot out.ppm | wait 30

The desktop reports its pointer position on the debug console ("P x y", kernel/display/wm_gfx.c), so
a move is closed in on until the pointer is exactly there; the same class is used by
tools/record-animations.py. Only for tests and recordings of the project's own builds.
"""
import os, socket, sys, time


class Drive:
    def __init__(self, sock, dbg):
        self.dbg = dbg
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(sock)
        self.s.settimeout(5)
        self._read()
        self.x = self.y = None
        self.idle = None                   # called instead of sleeping (the recorder takes frames then)

    def _read(self):
        try:
            return self.s.recv(65536)
        except Exception:
            return b''

    def sleep(self, secs):
        end = time.time() + secs
        while time.time() < end:
            if self.idle:
                self.idle()
            else:
                time.sleep(min(0.03, max(0, end - time.time())))

    def hmp(self, cmd, wait=0.05):
        self.s.send((cmd + '\n').encode())
        self.sleep(wait)
        return self._read()

    def pointer(self):
        """the last position the desktop reported, or None"""
        try:
            with open(self.dbg, 'rb') as f:
                f.seek(max(0, os.path.getsize(self.dbg) - 400))
                lines = [l for l in f.read().decode('latin1').split('\n') if l.startswith('P ')]
        except OSError:
            return None
        if not lines:
            return None
        p = lines[-1].split()
        try:
            return int(p[1]), int(p[2])
        except (IndexError, ValueError):
            return None

    def move(self, x, y, tries=60):
        """closes in on (x, y): the desktop speeds up big mouse movements, so the step is divided by the speed-up seen so far"""
        gain = getattr(self, 'gain', 1.0)
        for _ in range(tries):
            p = self.pointer()
            if p is None:
                self.hmp('mouse_move 1 1', 0.2)
                continue
            dx, dy = x - p[0], y - p[1]
            if dx == 0 and dy == 0:
                self.gain = gain
                return True
            step = lambda d: int(max(-60, min(60, d / gain)))
            rx, ry = step(dx), step(dy)
            if rx == 0 and ry == 0:
                rx, ry = (1 if dx > 0 else -1 if dx < 0 else 0), (1 if dy > 0 else -1 if dy < 0 else 0)
            self.hmp('mouse_move %d %d' % (rx, ry), 0.05)
            end = time.time() + 2.0                # (the desktop answers a move only when it gets to polling the mouse)
            while time.time() < end and self.pointer() == p:
                self.sleep(0.03)
            q = self.pointer() or p
            big = max(abs(rx), abs(ry))
            if big >= 8:
                seen = max(abs(q[0] - p[0]), abs(q[1] - p[1])) / big
                gain = max(1.0, min(8.0, 0.5 * gain + 0.5 * seen))
        self.gain = gain
        return False

    def glide(self, x, y, steps=24, delay=0.0):
        """a move in many small steps (for recordings)"""
        p = self.pointer()
        if p is None:
            return self.move(x, y)
        for i in range(1, steps + 1):
            self.move(p[0] + (x - p[0]) * i // steps, p[1] + (y - p[1]) * i // steps, tries=3)
        return self.move(x, y)

    def press(self):
        self.hmp('mouse_button 1', 0.1)

    def release(self):
        self.hmp('mouse_button 0', 0.1)

    def click(self):
        self.press()
        self.release()

    def key(self, k, wait=0.3, hold=150):
        """(held for a while: the desktop reads the keyboard between frames, and a tap can pass unseen)"""
        self.hmp('sendkey %s %d' % (k, hold), wait)

    KEYS = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus', '\n': 'ret', '=': 'equal', ',': 'comma', ';': 'semicolon', "'": 'apostrophe',
            '>': 'shift-dot', '<': 'shift-comma', '~': 'shift-grave_accent', ':': 'shift-semicolon', '"': 'shift-apostrophe', '_': 'shift-minus',
            '|': 'shift-backslash', '&': 'shift-7', '#': 'shift-3', '*': 'shift-8', '(': 'shift-9', ')': 'shift-0', '?': 'shift-slash', '$': 'shift-4'}

    def type(self, text, hold=60):
        for ch in text:
            k = self.KEYS.get(ch) or ('shift-' + ch.lower() if ch.isupper() else ch)
            self.key(k, 0.1, hold)

    def wait_serial(self, serial, text, timeout=600):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if text in open(serial, 'rb').read().decode('latin1'):
                    return True
            except OSError:
                pass
            self.sleep(1)
        return False

    def login(self, serial):
        """from the first boot to the desktop: Recovery, Try tOS in Live Environment, Start"""
        if not self.wait_serial(serial, 'session: recovery'):
            return False
        self.sleep(8)
        self.move(640, 425); self.click(); self.sleep(1.5)
        self.move(995, 640); self.click(); self.sleep(1.5)
        for _ in range(8):                      # (the Start page takes a moment to come: click until the desktop is there)
            self.sleep(4)
            self.click()
            if self.wait_serial(serial, 'session: desktop', 25):
                return True
        return False

    def shot(self, path):
        self.hmp('screendump ' + path, 0.15)


def main():
    d = Drive(sys.argv[1], sys.argv[2])
    a = sys.argv[3:]
    while a:
        c = a.pop(0)
        if c == 'move':
            print('moved', d.move(int(a.pop(0)), int(a.pop(0))), d.pointer())
        elif c == 'click':
            d.click()
        elif c == 'key':
            d.key(a.pop(0))
        elif c == 'type':
            d.type(a.pop(0))
        elif c == 'shot':
            d.shot(a.pop(0))
        elif c == 'wait':
            time.sleep(float(a.pop(0)))
        elif c == 'pos':
            print(d.pointer())


if __name__ == '__main__':
    main()
