#!/usr/bin/env python3
"""LICENSES.txt for the Linux runtime libraries in the initrd: the Debian/Ubuntu packages they
were copied from (dpkg), their versions, licences and where their sources are."""
import subprocess, sys
out = sys.argv[1]
pkgs = [('libc6', 'glibc', 'GNU Lesser General Public License 2.1 or later',
         'ld-linux-x86-64.so.2 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1\n                   libresolv.so.2 libnss_dns.so.2 libnss_files.so.2 (name lookups: getaddrinfo loads the last two)'),
        ('libstdc++6', 'gcc', 'GNU General Public License 3 with the GCC Runtime Library Exception 3.1',
         'libstdc++.so.6'),
        ('libgcc-s1', 'gcc', 'GNU General Public License 3 with the GCC Runtime Library Exception 3.1',
         'libgcc_s.so.1')]
def dpkg(field, pkg):
    try:
        return subprocess.check_output(['dpkg-query', '-W', '-f=${' + field + '}', pkg + ':amd64'],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ''
try:
    distro = open('/etc/os-release').read().split('PRETTY_NAME="')[1].split('"')[0]
except Exception:
    distro = 'the build machine'
lines = ['Linux runtime libraries in tOS', '==============================', '',
         'What 64-bit Linux programs (Node.js, opencode...) are linked against: the dynamic linker, the',
         'GNU C Library and the GCC runtime, copied unmodified from %s.' % distro, '']
for pkg, src, lic, files in pkgs:
    ver = dpkg('Version', pkg) or 'unknown'
    srcpkg = (dpkg('source:Package', pkg) or src)
    srcver = dpkg('source:Version', pkg) or ver
    lines += ['%s %s (source package %s %s)' % (pkg, ver, srcpkg, srcver),
              '  files:   ' + files,
              '  licence: ' + lic,
              '  source:  https://launchpad.net/ubuntu/+source/%s/%s' % (srcpkg, srcver),
              '           (also: apt-get source %s=%s)' % (srcpkg, srcver), '']
lines += ['The licence texts are beside this file (LICENSE.LGPL-2.1, LICENSE.GPL-3); the GCC Runtime',
          'Library Exception is at https://www.gnu.org/licenses/gcc-exception-3.1.html .', '']
open(out, 'w').write('\n'.join(lines))
