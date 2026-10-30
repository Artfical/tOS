#!/bin/sh
# OpenSSH for tOS (the openssh package): the release tarball plus tos.patch, built against the system's
# OpenSSL and zlib (the openssl and zlib packages carry the same libraries on tOS).
#
#   tools/openssh/build.sh <openssh-10.6p1.tar.gz> <work directory>
#
# Leaves the programs in <work directory>/openssh-10.6p1 and tos-sh, tos-sshd-run beside it. The package is
# then made with pkg/make_pkg.py (bin/: ssh scp sftp ssh-keygen ssh-add ssh-agent ssh-keyscan and tos-sh as
# sh; sbin/sshd; libexec/: sshd-session sshd-auth sftp-server and tos-sshd-run as sshd-run; etc/ssh_config,
# sshd_config; lib/libcrypt.so.1; an empty var/empty).
set -e
tarball=$1; work=$2
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$work"
tar xzf "$tarball" -C "$work"
cd "$work/openssh-10.6p1"
patch -p1 < "$here/tos.patch"
P=/system/pkg/openssh
./configure --prefix=$P --sysconfdir=$P/etc --libexecdir=$P/libexec --with-privsep-path=$P/var/empty \
    --without-pam --without-selinux --disable-strip --with-sandbox=no --disable-pkcs11 --disable-security-key \
    --with-ldflags="-Wl,-rpath,$P/lib:/system/pkg/openssl/lib:/system/pkg/zlib/lib" --with-ssl-dir=/usr --with-zlib=/usr --with-pid-dir=/tmp
make -j"$(nproc)"
gcc -O2 -static "$here/tos-sh.c" -o "$work/tos-sh"
gcc -O2 -static "$here/tos-sshd-run.c" -o "$work/tos-sshd-run"
