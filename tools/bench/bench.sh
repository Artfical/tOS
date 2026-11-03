#!/bin/bash
# tOS's speed on this machine: bench.sh <kvm|tcg> [host:port]  (the second one is a web server with /big.bin)
# Builds the self-test kernel with the benchmark program as its only extra run, boots it and prints the [BENCH] lines.
set -eu
accel=${1:-tcg}; url=${2:-}
here=$(cd "$(dirname "$0")" && pwd); root=$here/../..
cd "$root"
cp kernel/core/kernel.c /tmp/kernel.c.bench
trap 'cp /tmp/kernel.c.bench kernel/core/kernel.c' EXIT
if [ -n "$url" ]; then sed -i "s#{ \"/programs/benchtest\", 0 }#{ \"/programs/benchtest\", \"$url\", 0 }#" kernel/core/kernel.c; fi
make -s -j"$(nproc)" NODE_STATIC_SELFTEST=1 kernel/tOS.elf >/dev/null
cp kernel/tOS.elf /tmp/bench-$accel.elf
w=$(mktemp -d); mkdir -p "$w/programs"
gcc -O2 -static tools/linux-tests/benchtest.c -o "$w/programs/benchtest" 2>/dev/null
cp initrd.tar /tmp/bench-$accel.tar; tar --append --format=ustar -f /tmp/bench-$accel.tar -C "$w" programs
flags=(-accel tcg)
[ "$accel" = kvm ] && flags=(-accel kvm -cpu host)
log=/tmp/bench-$accel.log; rm -f "$log"
timeout 900 qemu-system-x86_64 "${flags[@]}" -kernel /tmp/bench-$accel.elf -initrd /tmp/bench-$accel.tar -m 1536 -vga std -nic user,model=e1000 -display none -serial file:"$log" -no-reboot >/dev/null 2>&1 &
q=$!
for i in $(seq 1 450); do grep -aq "\[BENCH\] done" "$log" 2>/dev/null && break; sleep 2; done
kill $q 2>/dev/null || true
echo "== $accel"; grep -a "\[BENCH\]" "$log" || { echo "no result"; tail -5 "$log"; }
