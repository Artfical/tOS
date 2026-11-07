#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/usr/bin:/bin:/system/pkg/busybox/bin
C="$P/coreutils/bin/coreutils --coreutils-prog"
cu() { n=$1; shift; [ $n = install ] && n=ginstall; $P/coreutils/bin/coreutils --coreutils-prog=$n "$@"; }
d=/tmp/cu; rm -rf $d; cu mkdir -p $d/a/b/c; cd $d
printf 'b 2\na 1\nc 3\na 1\n' > f.txt
chk "$(cu sort -u f.txt | cu tr '\n' ' ')" "a 1 b 2 c 3 " "sort -u"
chk "$(cu sort f.txt | cu uniq -c | cu head -1 | cu tr -s ' ')" " 2 a 1" "uniq -c"
chk "$(cu wc -l < f.txt)" 4 "wc -l"
chk "$(cu cut -d' ' -f2 f.txt | cu paste -sd+ | cu head -1)" "2+1+3+1" "cut and paste"
chk "$(cu seq 1 5 | cu tac | cu head -1)" 5 "seq and tac"
chk "$(printf abc | cu sha256sum | cu cut -c1-16)" ba7816bf8f01cfea "sha256sum"
chk "$(printf abc | cu md5sum | cu cut -c1-32)" 900150983cd24fb0d6963f7d28e17f72 "md5sum"
chk "$(printf abc | cu base64)" YWJj "base64"
chk "$(cu date -u -d @0 +%Y-%m-%dT%H:%M:%S)" 1970-01-01T00:00:00 "date -d"
chk "$(cu expr 6 \* 7)" 42 "expr"
cu cp -r a a2; cu ln -s a/b lnk; chk "$(cu readlink lnk)" a/b "ln -s and readlink"
cu install -m 640 f.txt a/inst.txt; chk "$(cu stat -c '%a %s' a/inst.txt)" "640 16" "install and stat"
chk "$(cu ls -1 a a2 | cu tr '\n' ' ')" "a: b inst.txt  a2: b " "ls -1 of two folders" 
cu dd if=/dev/zero of=z.bin bs=1k count=64 2>/dev/null; chk "$(cu stat -c %s z.bin)" 65536 "dd"
chk "$(cu od -An -tx1 -N4 z.bin | cu tr -d ' ')" 00000000 "od"
cu truncate -s 100 z.bin; chk "$(cu stat -c %s z.bin)" 100 "truncate"
chk "$(cu basename /x/y/z.txt .txt) $(cu dirname /x/y/z.txt)" "z /x/y" "basename dirname"
chk "$(cu env FOO=bar $P/busybox/bin/busybox sh -c 'echo $FOO')" bar "env"
chk "$(cu printf '%s-%03d' ab 7)" ab-007 "printf"
chk "$(cu timeout 1 $P/busybox/bin/busybox sleep 5; echo $?)" 124 "timeout"
chk "$(cu nproc >/dev/null; echo ok)" ok "nproc"
chk "$(cu mktemp -d | cu cut -c1-5)" /tmp/ "mktemp"
printf "a\nb\n" > c1; printf "b\nc\n" > c2; chk "$(cu comm -12 c1 c2)" b "comm"
chk "$(cu fold -w3 < f.txt | cu head -1)" "b 2" "fold"
chk "$(echo hi | cu tee t.out >/dev/null; cat t.out)" hi "tee"
chk "$(cu shuf -n1 f.txt | cu wc -l)" 1 "shuf"
chk "$(cu numfmt --to=iec 2048)" 2.0K "numfmt"
chk "$(cu sync; echo ok)" ok "sync"
chk "$(cu id -u)" 0 "id"
cu rm -rf $d; chk "$(cu test -e $d && echo yes || echo no)" no "rm -rf"
