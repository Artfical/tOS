title: Go
lede: Go 1.23 is not yet packaged for tOS, but static Go binaries run unmodified. Cross-compilation from Linux works today.
---
## Current status

Go (`go`, `gofmt`, `golint`, etc.) is **not yet a tpkg package**. The Go toolchain is written in Go and bootstraps itself; porting it requires a working Go compiler first.

## Running Go programs on tOS

Go compiles to static binaries by default (when `CGO_ENABLED=0`). These run on tOS without any Go installation:

```go
// hello.go
package main
import (
    "fmt"
    "net/http"
)

func main() {
    http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
        fmt.Fprintln(w, "hello from Go on tOS")
    })
    http.ListenAndServe(":8080", nil)
}
```

```bash
# On your Linux host
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o myserver hello.go
scp myserver tos@192.168.x.x:/tmp/
```

On tOS:

```bash
/tmp/myserver
# Then from host: curl http://tos-ip:8080
```

## Cross-compilation

```bash
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o myapp main.go
```

The resulting binary is statically linked (no glibc dependency) and runs on any Linux x86-64, including tOS.

## What works in Go on tOS

| Feature | Status |
|---|---|
| Standard library | ✅ Full (net, http, crypto, encoding, etc.) |
| `net/http` servers/clients | ✅ TLS 1.2 supported |
| `database/sql` + SQLite | ✅ With `github.com/mattn/go-sqlite3` (CGO) |
| `net/rpc`, `net/rpc/jsonrpc` | ✅ |
| `html/template`, `text/template` | ✅ |
| `compress/*`, `archive/*` | ✅ |
| `plugin` package | ❌ (needs dynamic loading) |
| `runtime/cgo` | ⚠️ Only with CGO_ENABLED=1 (needs gcc) |

## CGO and C libraries

If you need CGO (e.g. `github.com/mattn/go-sqlite3`), you must:

1. Cross-compile with a C cross-compiler (not available yet on tOS).
2. Or build on a Linux host with `gcc` and copy the binary.

Pure Go packages (no CGO) work out of the box.

## Modules and `go.mod`

Go modules work normally. `go mod download`, `go mod tidy`, `go mod vendor` all work on the build host. The resulting binary has no external dependencies.

## Private modules

Use `GOPRIVATE` or `.netrc` for private modules. The `go` command uses the same environment variables on the build host.

## Related

- [Developing on tOS](/tos/developing)
- [Networking](/tos/networking)
- [Linux programs](/tos/linux-programs)