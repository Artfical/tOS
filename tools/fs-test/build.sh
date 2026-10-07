#!/bin/bash
# usage: build.sh FS NAMEMODE   (FS = ntfs ext2 ext3 ext4 fat16 fat32 exfat btrfs xfs)
fs=$1; mode=$2; here=$(cd "$(dirname "$0")" && pwd)
extra=""; [ "$fs" = ntfs ] && extra="-DFS_IS_NTFS"
case $fs in ext2|ext3|ext4|exfat|fat16|fat32) extra="$extra -DFS_HAS_FORMAT";; esac
[ "$fs" = ext4 ] && extra="$extra -DFS_HAS_MKFS"
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wno-unused-function -Wno-format-truncation -Wno-unused-variable \
  -DFS=$fs -DFS_SRC="\"$fs.c\"" -DNAMEMODE=$mode $extra -I "$here/shim" -I "$here/../../kernel/fs" "$here/fs_host.c" -o /tmp/fs_host_$fs 2>&1 | grep -E "error|Error" | head -20
ls -la /tmp/fs_host_$fs 2>&1 | awk '{print $5, $9}'
