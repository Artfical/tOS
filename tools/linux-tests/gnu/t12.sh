#!/system/pkg/busybox/bin/busybox sh
# gdb and strace (ptrace): a program built with -g stops at a breakpoint, finishes a call, and its system calls are listed
[ -x /system/pkg/gdb/bin/gdb-tos ] || exit 0
[ -x /system/pkg/gcc/bin/gcc ] || exit 0
chk() { if [ "$1" = "$2" ]; then echo "dbg: ok $3"; else echo "dbg: FAIL $3 (got [$1] want [$2])"; fi; }
export HOME=/root PATH=/usr/bin:/bin:/system/pkg/busybox/bin:/system/pkg/gcc/bin:/system/pkg/binutils/bin:/system/pkg/make/bin
cd /tmp && cp /programs/gt/dbg_t.c . && gcc -g -O0 dbg_t.c -o dbg_t
cat > dbg_cmds <<'XX'
set pagination off
set confirm off
break square
run
bt
finish
next
print total
continue
XX
out=$(busybox sh /system/pkg/gdb/bin/gdb-tos -q -batch -x dbg_cmds ./dbg_t 2>&1)
chk "$(echo "$out" | grep -c 'Breakpoint 1, square (n=1)')" 1 gdb-breakpoint
chk "$(echo "$out" | grep -c '#1  0x[0-9a-f]* in main ()')" 1 gdb-backtrace
chk "$(echo "$out" | grep -c 'Value returned is .1 = 1')" 1 gdb-finish
chk "$(echo "$out" | grep -c 'Breakpoint 1, square (n=2)')" 1 gdb-second-hit
if [ -x /system/pkg/strace/bin/strace ]; then
  /system/pkg/strace/bin/strace -o /tmp/dbg_st.txt ./dbg_t > /dev/null
  chk "$(grep -c 'write(1, "total=14' /tmp/dbg_st.txt)" 1 strace-write
  chk "$(grep -c '^execve' /tmp/dbg_st.txt)" 1 strace-execve
fi
