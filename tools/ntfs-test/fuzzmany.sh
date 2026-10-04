#!/bin/bash
# usage: fuzzmany.sh FIRST_SEED LAST_SEED NOPS BASEIMG  -> runs each seed, checks image with the python checker
first=$1; last=$2; nops=$3; base=${4:?need a base mkntfs image}
fails=0
for seed in $(seq $first $last); do
  img=/tmp/fz_$seed.img; cp $base $img
  out=$(ASAN_OPTIONS=detect_leaks=0 timeout 900 ./ntfs_host $img fuzz $seed $nops 2>&1 | tail -3)
  if ! echo "$out" | grep -q "FUZZ: ok"; then echo "seed $seed: HARNESS FAIL: $(echo "$out" | head -2 | tr '\n' ' ' | cut -c1-200)"; fails=$((fails+1)); continue; fi
  chk=$(python3 $(dirname "$0")/ntfscheck.py $img 2>&1 | tail -3)
  if ! echo "$chk" | tail -1 | grep -q " 0 errors"; then echo "seed $seed: CHECKER: $(echo "$chk" | tr '\n' ' ' | cut -c1-250)"; fails=$((fails+1)); else rm -f $img; fi
done
echo "fuzzmany done: seeds $first..$last, $fails failures"
