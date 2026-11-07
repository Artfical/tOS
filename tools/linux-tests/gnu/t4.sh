#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/usr/bin:/bin:/system/pkg/busybox/bin
cd /tmp; printf 'one two\nthree four\nfive six\n' > s.txt
S=$P/sed/bin/sed; G=$P/grep/bin/grep; A=$P/gawk/bin/gawk
chk "$($S -E 's/(\w+) (\w+)/\2 \1/' s.txt | head -1)" "two one" "sed -E with groups"
cp s.txt s2.txt; $S -i 's/o/0/g' s2.txt; chk "$(head -1 s2.txt)" "0ne tw0" "sed -i"
chk "$($S -n '2p' s.txt)" "three four" "sed -n 2p"
chk "$($S '2,3d' s.txt)" "one two" "sed range delete"
chk "$(printf 'a\nb\n' | $S ':a;N;$!ba;s/\n/,/g')" "a,b" "sed N loop"
chk "$($G -c o s.txt)" 2 "grep -c"
chk "$($G -oP '(?<=th)\w+' s.txt)" ree "grep -P lookbehind"
chk "$($G -E '^(one|five)' s.txt | wc -l | tr -d ' ')" 2 "grep -E"
chk "$($G -rl four /tmp/s.txt)" /tmp/s.txt "grep -r"
chk "$(echo abc | $G -v x; echo rc=$?)" "abc
rc=0" "grep -v"
chk "$($G -i ONE s.txt)" "one two" "grep -i"
chk "$($A '{n+=NF} END{print n}' s.txt)" 6 "awk NF sum"
chk "$($A -F'[ ]' '{a[$1]=length($2)} END{for(k in a) s+=a[k]; print s}' s.txt)" 10 "awk assoc array"
chk "$($A 'BEGIN{printf "%05.1f %s\n", 3.14159, toupper("x")}')" "003.1 X" "awk printf"
chk "$($A 'BEGIN{print strftime("%Y", 0, 1)}')" 1970 "gawk strftime"
chk "$($A 'BEGIN{print gensub(/(a)(b)/, "\\2\\1", "g", "abab")}')" baba "gawk gensub"
chk "$(echo 5 3 | $A '{print $1 ** $2, sqrt(16), int(7/2)}')" "125 4 3" "awk math"
chk "$($A --version | head -1 | cut -c1-9)" "GNU Awk 5" "gawk version"
D=$P/findutils/bin; mkdir -p /tmp/fd/x/y; touch /tmp/fd/x/a.c /tmp/fd/x/y/b.c /tmp/fd/x/y/c.h
chk "$($D/find /tmp/fd -name '*.c' | sort | tr '\n' ' ')" "/tmp/fd/x/a.c /tmp/fd/x/y/b.c " "find -name"
chk "$($D/find /tmp/fd -type f -name '*.c' -exec echo {} + | wc -w | tr -d ' ')" 2 "find -exec +"
chk "$($D/find /tmp/fd -type f | $D/xargs -n1 basename | sort | tr '\n' ' ')" "a.c b.c c.h " "find | xargs"
chk "$($D/find /tmp/fd -maxdepth 1 -type d | wc -l | tr -d ' ')" 2 "find -maxdepth"
chk "$($D/find /tmp/fd -newer /tmp/s.txt -type f | wc -l | tr -d ' ')" 3 "find -newer"
printf 'a\nb\nc\n' > d1; printf 'a\nB\nc\n' > d2
Dd=$P/diffutils/bin
chk "$($Dd/diff d1 d2 | head -1)" 2c2 "diff"
chk "$($Dd/cmp d1 d2 2>&1 | cut -c1-30)" "d1 d2 differ: char 3, line 2" "cmp"
$Dd/diff -u d1 d2 > d.patch; cp d1 d3; $P/patch/bin/patch d3 < d.patch >/dev/null; chk "$(cat d3 | tr '\n' ' ')" "a B c " "diff -u | patch"
printf "a\nX\nc\n" > m2; printf "a\nY\nc\n" > m3; chk "$($Dd/diff3 -m d1 m2 m3 >/dev/null; echo $?)" 1 "diff3 finds a conflict"
