#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/usr/bin:/bin:/system/pkg/busybox/bin
cat > /tmp/b1.sh <<'XX'
#!/system/pkg/bash/bin/bash
set -eu
declare -A m; m[a]=1; m[b]=2
arr=(x y "z w"); echo "n=${#arr[@]} last=${arr[-1]} keys=$(for k in "${!m[@]}"; do echo $k; done | sort | tr -d '\n')"
f() { local i=$1; (( i > 1 )) && echo $(( i * $(f $((i-1))) )) || echo 1; }
echo "fact=$(f 5)"
s="Hello World"; echo "${s,,} ${s^^} ${s#Hello } ${s/World/tOS} ${#s}"
while read -r l; do echo "l:$l"; done < <(printf 'a\nb\n')
cat <<EOT
heredoc $((6*7))
EOT
trap 'echo trapped' EXIT
[[ $s =~ ^H(.*)d$ ]] && echo "re=${BASH_REMATCH[1]}"
echo "glob=$(echo /system/pkg/bash/b*)"
printf '%05.1f|%-4s|%x\n' 3.14159 ab 255
for i in {1..3}; do echo -n "$i,"; done; echo
XX
out=$($P/bash/bin/bash /tmp/b1.sh 2>&1)
echo "$out" | sed 's/^/  /'
chk "$(echo "$out" | grep -c .)" 11 "bash script output lines"
chk "$(echo "$out" | grep fact)" "fact=120" "bash recursion and arithmetic"
chk "$($P/bash/bin/bash -c 'echo $BASH_VERSION')" "5.2.37(1)-release" "bash version"
chk "$($P/bash/bin/bash -c 'set -o pipefail; false | true; echo $?')" 1 "pipefail"
chk "$($P/bash/bin/bash -c 'x=$(echo sub); echo $x; exit 3'; echo " rc=$?")" "sub
 rc=3" "bash subshell and exit status"
