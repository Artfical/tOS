#!/bin/sh
# The initrd the kernel's self-test (make NODE_STATIC_SELFTEST=1) runs against: the repository's own
# initrd.tar plus the Linux programs it exercises (fork/exec, the terminal, Node 8 and 24, Git).
# Node and Git come from the update server's packages, as a user's tOS gets them.
#
#   tools/linux-tests/build-initrd.sh <initrd.tar> <output.tar> [package directory]
#
# Needs: gcc, tar; the package directory (default /root/tos-update/pkg) holds <name>/<name>-<version>.tpkg.
set -e
base=$1; out=$2; pkgs=${3:-/root/tos-update/pkg}
here=$(cd "$(dirname "$0")" && pwd)
w=$(mktemp -d)
mkdir -p "$w/programs/js" "$w/n8" "$w/n24/node24" "$w/sys/system/pkg"
gcc -O2 "$here/forktest.c" -o "$w/programs/forktest" 2>/dev/null
gcc -O2 -static "$here/ttytest.c" -o "$w/programs/ttytest" 2>/dev/null
gcc -O2 -static "$here/vtdemo.c" -o "$w/programs/vtdemo" 2>/dev/null
cp "$here/t2.js" "$here/cp.js" "$w/programs/js/"
printf '#!/programs/forktest arg\n' > "$w/programs/ft.sh"; chmod +x "$w/programs/ft.sh"
# "#!/usr/bin/env name" scripts: one name in path.tmbl, one in an installed package's bin folder
printf '#!/usr/bin/env ftcmd\n' > "$w/programs/ft-env.sh"; chmod +x "$w/programs/ft-env.sh"
printf '#!/usr/bin/env ftpkgcmd\n' > "$w/programs/ft-env2.sh"; chmod +x "$w/programs/ft-env2.sh"
mkdir -p "$w/sys/system/pkg/ftpkg/bin"
cp "$w/programs/forktest" "$w/sys/system/pkg/ftpkg/bin/ftpkgcmd"
printf 'ftcmd=/programs/forktest\n' > "$w/sys/system/path.tmbl"
printf 'ftpkg|1|/system/pkg/ftpkg\n' > "$w/sys/system/tpkg_db.tmbl"
tar xzf "$pkgs"/node/node-8.17.0-2.tpkg -C "$w/n8"          # bin/node, npm/
cp "$w/n8/bin/node" "$w/programs/node"
mkdir -p "$w/n24/x"; tar xzf "$pkgs/node24/$(sed -n 's/.*"file": "\(.*\)".*/\1/p' "$pkgs/node24/meta.json")" -C "$w/n24/x"
cp -a "$w/n24/x/bin" "$w/n24/node24/"
for p in git curl openssl zlib ca-certificates; do
    mkdir -p "$w/sys/system/pkg/$p"
    f=$(sed -n 's/.*"file": "\(.*\)".*/\1/p' "$pkgs/$p/meta.json")     # the version the server serves now
    tar xzf "$pkgs/$p/$f" -C "$w/sys/system/pkg/$p"
done
cp "$base" "$out"
tar --append --format=ustar -f "$out" -C "$w" programs
tar --append --format=ustar -f "$out" -C "$w/n8" npm
tar --append --format=ustar -f "$out" -C "$w/n24" node24
tar --append --format=ustar -f "$out" -C "$w/sys" system
rm -rf "$w"
echo "$out: $(du -h "$out" | cut -f1)"
