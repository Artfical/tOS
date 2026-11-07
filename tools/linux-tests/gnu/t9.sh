#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/usr/bin:/bin:/system/pkg/busybox/bin
W=$P/wget/bin/wget
chk "$($W --version | head -1 | cut -c1-18)" "GNU Wget 1.24.5 bu" "wget version"
chk "$($W --help | grep -c https | sed s/2/1/)" 1 "wget has https"
cd /tmp; rm -rf w; mkdir w; cd w
$P/python3/bin/python3.14 -u /programs/gt/tlssrv.py > srv.log 2>&1 &
SP=$!
i=0; while [ $i -lt 60 ] && ! grep -q listening srv.log; do sleep 2; i=$((i+1)); done
chk "$(grep -c listening srv.log)" 1 "a local TLS server is up"
$W -q -O out.html --ca-certificate=/programs/gt/c.pem https://127.0.0.1:8443/; chk "$?" 0 "wget HTTPS with the server certificate as the CA"
chk "$(cat out.html)" "hello tls from tOS" "the page came through"
$W -q -O out2.html https://127.0.0.1:8443/; chk "$?" 5 "wget refuses a certificate no CA vouches for (exit 5)"
$W -q -O out3.html --no-check-certificate https://127.0.0.1:8443/; chk "$?" 0 "wget --no-check-certificate"
chk "$($W -q -O - --ca-certificate=/programs/gt/c.pem https://localhost:8443/)" "hello tls from tOS" "wget by host name (localhost)"
$W -S -q -O /dev/null --ca-certificate=/programs/gt/c.pem https://127.0.0.1:8443/ 2>&1 | grep -c "HTTP/1.0 200" | { read v; chk $v 1 "wget -S shows the response"; }
kill $SP 2>/dev/null
