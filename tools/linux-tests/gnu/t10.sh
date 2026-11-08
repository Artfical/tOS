#!/system/pkg/busybox/bin/busybox sh
# Go: the compiler builds and runs programs (the standard library comes from the package's ready-made build cache)
[ -x /system/pkg/go/bin/go-tos ] || exit 0
chk() { if [ "$1" = "$2" ]; then echo "go: ok $3"; else echo "go: FAIL $3 (got [$1] want [$2])"; fi; }
export HOME=/root GOPATH=/tmp/gopath PATH=/usr/bin:/bin:/system/pkg/busybox/bin:/system/pkg/go/bin
chk "$(go version | cut -d' ' -f1-3)" "go version go1.27.3" version
mkdir -p /tmp/gob && cd /tmp/gob && cp /programs/gt/go_hello.go hello.go && cp /programs/gt/go_rt.go rt.go
go build -o hello hello.go
chk "$(./hello)" "hello from Go on tOS" build-run
chk "$(go run rt.go | grep -c 'goroutines sum 2799989')" 1 goroutines
chk "$(gofmt -l . | wc -l)" 1 gofmt-lists-unformatted
