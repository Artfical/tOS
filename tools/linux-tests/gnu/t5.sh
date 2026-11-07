#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/usr/bin:/bin:/system/pkg/busybox/bin
cd /tmp; rm -rf tz; mkdir -p tz/src/sub; cd tz
i=0; while [ $i -lt 200 ]; do echo "line $i of the file with some repeating text text text" ; i=$((i+1)); done > src/big.txt
echo hello > src/sub/h.txt; ln -s h.txt src/sub/lnk
T=$P/tar/bin/tar; Z=$P/gzip/bin/gzip
$T -cf a.tar src; chk "$($T -tf a.tar | wc -l | tr -d ' ')" 5 "tar create and list"
$T -czf a.tgz src; chk "$($T -tzf a.tgz | wc -l | tr -d ' ')" 5 "tar -z (gzip through the gzip package)"
$T -cjf a.tbz src; chk "$($T -tjf a.tbz | wc -l | tr -d ' ')" 5 "tar -j (bzip2)"
mkdir out; $T -xzf a.tgz -C out; chk "$(cat out/src/sub/h.txt)" hello "tar extract"
chk "$(readlink out/src/sub/lnk)" h.txt "tar keeps symlinks"
chk "$(diff -r src out/src && echo same)" same "tar round trip is identical"
$Z -k -9 src/big.txt; chk "$($Z -l src/big.txt.gz | tail -1 | tr -s ' ' | cut -d' ' -f2)" "$(wc -c < src/big.txt.gz)" "gzip -l matches the size"
$Z -dc src/big.txt.gz | cmp - src/big.txt && echo "gt: ok gzip -dc round trip" || echo "gt: FAIL gzip -dc round trip"
chk "$($P/gzip/bin/zcat src/big.txt.gz | wc -l | tr -d ' ')" 200 "zcat"
gunzip_out=$($P/gzip/bin/gunzip -c src/big.txt.gz | wc -c | tr -d ' '); chk "$gunzip_out" "$(wc -c < src/big.txt)" "gunzip -c"
chk "$(cat src/sub/h.txt | $Z | $Z -d)" hello "gzip pipe"
chk "$($P/file/bin/file -b src/big.txt)" "ASCII text" "file: ascii text"
chk "$($P/file/bin/file -b --mime-type src/big.txt.gz)" application/gzip "file: gzip"
chk "$($P/file/bin/file -b --mime-type $P/perl/bin/perl)" application/x-pie-executable "file: ELF"
chk "$($P/file/bin/file -b --mime-type a.tar)" application/x-tar "file: tar"
chk "$($P/file/bin/file -b src/sub/lnk | cut -c1-13)" "symbolic link" "file: symlink"
chk "$(cat src/big.txt | $P/less/bin/less | wc -l | tr -d ' ')" 200 "less as cat in a pipe"
chk "$($P/less/bin/less --version | head -1 | cut -c1-9)" "less 668 " "less version"
mkdir -p rs/dst; $P/rsync/bin/rsync -a src/ rs/dst/; chk "$(diff -r src rs/dst && echo same)" same "rsync -a local"
echo more > src/new.txt; $P/rsync/bin/rsync -a --delete --stats src/ rs/dst/ | grep -c "Number of created files: 1" | { read v; chk $v 1 "rsync incremental copies only the new file"; }
rm src/new.txt; $P/rsync/bin/rsync -a --delete src/ rs/dst/; chk "$(ls rs/dst | tr '\n' ' ')" "big.txt big.txt.gz sub " "rsync --delete"
chk "$(echo 'X+X' | $P/m4/bin/m4 -DX=1)" "1+1" "m4 -D"
chk "$($P/pkgconf/bin/pkg-config --version)" 2.3.0 "pkg-config version"
chk "$($P/pkgconf/bin/pkg-config --cflags zlib 2>&1)" "-I/system/pkg/zlib-dev/include" "pkg-config --cflags zlib"
chk "$($P/pkgconf/bin/pkg-config --libs zlib)" "-L/system/pkg/zlib-dev/lib -lz" "pkg-config --libs zlib"
chk "$($P/pkgconf/bin/pkg-config --modversion openssl | cut -c1-2)" 3. "pkg-config openssl"
chk "$($P/pkgconf/bin/pkg-config --exists zlib '>=' 1.0 && echo y)" y "pkg-config --exists version"
chk "$($P/ninja/bin/ninja --version)" 1.12.1 "ninja version"
chk "$($P/cmake/bin/cmake --version | head -1)" "cmake version 3.30.5" "cmake version"
