#!/usr/bin/env python3
"""Every ELF file under the given package trees: are its NEEDED libraries found where tOS will look (its RUNPATH folders, the libc of the runtime)?
   checklibs.py ROOT   (ROOT holds the packages, as /system/pkg does)"""
import os, subprocess, sys
root = sys.argv[1]
libc = {'libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1', 'ld-linux-x86-64.so.2', 'libutil.so.1', 'libresolv.so.2', 'libnsl.so.1', 'libanl.so.1'}
def tr(p): return os.path.join(root, p[len('/system/pkg/'):]) if p.startswith('/system/pkg/') else '/nonexistent' + p
bad = {}
for pkg in sys.argv[2:]:
    for dp, dn, fn in os.walk(os.path.join(root, pkg)):
        for f in fn:
            p = os.path.join(dp, f)
            if os.path.islink(p) or not os.path.isfile(p): continue
            with open(p, 'rb') as fh:
                if fh.read(4) != b'\x7fELF': continue
            try:
                need = subprocess.check_output(['patchelf', '--print-needed', p], text=True, stderr=subprocess.DEVNULL).split()
                rp = subprocess.check_output(['patchelf', '--print-rpath', p], text=True, stderr=subprocess.DEVNULL).strip().split(':')
            except Exception:
                continue
            dirs = [tr(d) for d in rp if d]
            for n in need:
                if n in libc: continue
                if any(os.path.exists(os.path.join(d, n)) for d in dirs): continue
                bad.setdefault(n, set()).add(os.path.relpath(p, root).split('/')[0] + ':' + os.path.basename(p))
for n, who in sorted(bad.items()):
    print('MISSING', n, '<-', ', '.join(sorted(who)[:4]), '(%d files)' % len(who))
print('checked; %d libraries not found' % len(bad))
