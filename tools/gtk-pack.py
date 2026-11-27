#!/usr/bin/env python3
"""Packs what build.sh installed under /system/pkg into tpkg packages (in /root/gtk-build/out/<name>/, not on the server):
     pack.py xlibs libpng gtk3"""
import json, os, shutil, subprocess, sys, tarfile

B = '/root/gtk-build'
SRC = B + '/src'
OUT = B + '/out'
os.makedirs(OUT, exist_ok=True)
MAKE = B + '/make_pkg.py'
if not os.path.exists(MAKE):
    shutil.copy('/root/tos-update/pkg/make_pkg.py', MAKE)

def lic(tarball, member_names, prefix):
    """licence files out of a source tarball -> [[file, name]]"""
    res = []
    with tarfile.open(SRC + '/' + tarball) as t:
        names = {os.path.basename(m.name): m for m in t.getmembers() if m.isfile() and m.name.count('/') == 1}
        for want in member_names:
            if want in names:
                dst = '%s/lic-%s-%s' % (B, prefix, want)
                with open(dst, 'wb') as f:
                    f.write(t.extractfile(names[want]).read())
                res.append([dst, 'LICENSE.%s' % prefix if want in ('COPYING', 'LICENSE') else 'LICENSE.%s.%s' % (prefix, want)])
    return res

def stage(name, drop=()):
    d = '%s/stage/%s' % (B, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.dirname(d), exist_ok=True)
    subprocess.check_call(['cp', '-rL', '/system/pkg/' + name, d])           # no symlinks in a tpkg
    for rel in ('share/man', 'share/doc', 'share/gtk-doc', 'share/locale', 'share/info', 'share/gdb', 'share/bash-completion', 'share/aclocal', 'share/gettext', 'lib/cmake', 'share/gir-1.0') + tuple(drop):
        shutil.rmtree(os.path.join(d, rel), ignore_errors=True)
    for root, dirs, files in os.walk(d):
        for f in files:
            if f.endswith(('.a', '.la')):
                os.remove(os.path.join(root, f))
    return d

def add_runpath(d, extra):
    """the build machine had libxcb-render in xorg; on tOS it is in xcb-libs: every ELF file also looks there"""
    for root, dirs, files in os.walk(d):
        for f in files:
            p = os.path.join(root, f)
            with open(p, 'rb') as fh:
                if fh.read(4) != b'\x7fELF': continue
            try:
                cur = subprocess.check_output(['patchelf', '--print-rpath', p], text=True, stderr=subprocess.DEVNULL).strip()
            except Exception:
                continue
            if cur and extra not in cur.split(':'):
                subprocess.check_call(['patchelf', '--set-rpath', cur + ':' + extra, p])

def make(spec):
    p = '%s/spec-%s.json' % (B, spec['name'])
    json.dump(spec, open(p, 'w'), indent=1)
    subprocess.check_call(['python3', MAKE, p], cwd=B)

what = sys.argv[1:]

if 'libpng' in what:
    make(dict(name='libpng', version='1.6.44', license='libpng-2.0', homepage='http://www.libpng.org/pub/png/libpng.html', min_build=636,
              desc='libpng 1.6.44, the PNG library (what GTK, cairo and Firefox read pictures with)', stage=stage('libpng', ('bin',)),
              depends=['zlib'], licenses=lic('libpng-1.6.44.tar.xz', ['LICENSE'], 'libpng'),
              notice='libpng 1.6.44 (libpng licence), built for tOS. Source: libpng-1.6.44.tar.xz beside this package.', sources=[[SRC + '/libpng-1.6.44.tar.xz', 'libpng-1.6.44.tar.xz']]))

if 'xlibs' in what:
    libs = ['libXfixes-6.0.1', 'libXi-1.8.2', 'libXrandr-1.5.4', 'libXcursor-1.2.3', 'libXinerama-1.1.5', 'libXdamage-1.1.6', 'libXcomposite-0.4.6', 'libXtst-1.2.5']
    lics = []
    for l in libs:
        lics += lic(l + '.tar.xz', ['COPYING'], l.split('-')[0])
    lics += lic('xorgproto-2024.1.tar.xz', ['COPYING-x11proto'], 'xorgproto') if False else []
    make(dict(name='xlibs', version='2024.11', license='MIT', homepage='https://www.x.org', min_build=636,
              desc='More X11 client libraries: Xfixes, Xi, Xrandr, Xcursor, Xinerama, Xdamage, Xcomposite, Xtst (with the X protocol headers). GTK and Firefox use them.',
              stage=stage('xlibs'), depends=['xorg'], licenses=lics,
              notice='X.Org client libraries built for tOS (MIT licence, each with its copyright notice in its LICENSE file). Sources beside this package.',
              sources=[[SRC + '/' + l + '.tar.xz', l + '.tar.xz'] for l in libs] + [[SRC + '/xorgproto-2024.1.tar.xz', 'xorgproto-2024.1.tar.xz']]))

if 'gtk3' in what:
    tars = {'glib': 'glib-2.80.5.tar.xz', 'gtk': 'gtk+-3.24.43.tar.xz', 'cairo': 'cairo-1.18.2.tar.xz', 'pango': 'pango-1.54.0.tar.xz', 'harfbuzz': 'harfbuzz-9.0.0.tar.xz',
            'fribidi': 'fribidi-1.0.15.tar.xz', 'gdk-pixbuf': 'gdk-pixbuf-2.42.12.tar.xz', 'atk': 'atk-2.38.0.tar.xz', 'libepoxy': 'libepoxy-1.5.10.tar.xz'}
    lics = []
    for k, t in tars.items():
        lics += lic(t, ['COPYING', 'COPYING-LGPL-2.1', 'COPYING-MPL-1.1', 'LICENSE'], k)
    make(dict(name='gtk3', version='3.24.43', license='LGPL-2.1-or-later (GTK, GLib, Pango, GdkPixbuf, ATK, Cairo also MPL-1.1, HarfBuzz and FriBidi, libepoxy MIT)',
              homepage='https://www.gtk.org', min_build=636,
              desc='GTK 3.24.43 with GLib 2.80, Cairo 1.18, Pango 1.54, HarfBuzz 9, FriBidi, GdkPixbuf, ATK and libepoxy, for the X11 backend (tos-x). No D-Bus, no accessibility bridge (a stub). With headers and pkg-config files.',
              stage=(lambda d: (add_runpath(d, '/system/pkg/xcb-libs/lib'), d)[1])(stage('gtk3')), depends=['xorg', 'xcb-libs', 'xlibs', 'libpng', 'fontconfig', 'expat', 'pcre2', 'libffi', 'zlib', 'xkbcommon', 'xcb-util', 'fonts-dejavu'], licenses=lics,
              notice='GTK and the libraries under it, built for tOS, with their licences in the LICENSE.* files. The sources they were built from are beside this package. atk-bridge-2.0 is a stub made for tOS.',
              sources=[[SRC + '/' + t, t] for t in tars.values()]))
