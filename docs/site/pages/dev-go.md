title: Go
lede: Go 1.27 runs on tOS: the compiler, the go command and the whole standard library, from a tpkg package. Build and run programs on tOS itself.
---
## Install

```
sudo tpkg install go
go version
```

`go version` answers `go version go1.27.3 linux/amd64`. The package is about 100 MB to download and about 370 MB on disk (tOS keeps files in memory, so give the machine 2 GB or more). It brings two commands: `go` and `gofmt`. It needs `sudo` only to install; running `go` needs no root.

> **sudo:** `sudo tpkg install go` (and `sudo tpkg remove go`) change the system, so they need root. Everything else on this page runs as your normal user.

## Your first program

```
mkdir hello && cd hello
cat > hello.go <<'EOF'
package main

import "fmt"

func main() { fmt.Println("hello from Go on tOS") }
EOF
go build -o hello hello.go
./hello
```

`go run hello.go` builds and runs in one step. `gofmt -l .` lists the files that are not formatted.

## The first run and the build cache

Go compiles the standard library the first time a program needs it, and keeps the result in a build cache. On a fresh machine that takes minutes. To spare you that, the package carries a **ready-made cache** of the whole standard library (built for `CGO_ENABLED=0` with the same compiler). The `go` command copies it to `~/.cache/go-build` the first time you run it, so that the first `go build` only compiles your own code.

- The copy is about 180 MB and takes a while on a slow machine. It happens once.
- `GOCACHE` changes the place. `go clean -cache` empties it (the next build then compiles the library again, slowly).
- Packages you download or write are compiled on first use and cached like the rest.

## What was tested

These ran on tOS (in a QEMU virtual machine) and worked:

| Test | Result |
|---|---|
| `go version` | go1.27.3 linux/amd64 |
| `go build` of a hello-world program, then running it | works |
| `go run` of a program with 8 goroutines, a channel, a timer, a map, sha256, file write and read, a 64 MB allocation and a forced garbage collection | works |
| `gofmt -l` | works |
| `go build` of a program that uses `net/http`, then an HTTPS request to example.com | status 200 |

Not tested yet: `go get` and modules from the internet, cgo (programs that use C code), `go test`, `go vet`, and the race detector. They may work; this page does not promise it.

## HTTPS from your own Go programs

A Go program looks for the CA certificates in the usual Linux places. On tOS the bundle is in the `ca-certificates` package, so point Go at it:

```
export SSL_CERT_FILE=/system/pkg/ca-certificates/cacert.pem
./myserver-or-client
```

The `go` command sets this for itself and for `go run`.

## cgo

When the `gcc` package is installed, Go switches cgo on by itself, the same as on Linux. A program that imports `net` or `os/user` then needs the C parts compiled, which is slower than the pure Go build that the ready-made cache holds. To get the quick build, run `CGO_ENABLED=0 go build`.

## Threads and the machine

A Go program starts several operating system threads, and tOS supports them (clone with a thread stack, futexes, `sigaltstack`, `waitid`). Go sees one CPU on tOS (`runtime.NumCPU()` is 1 today), so goroutines share one core.

## Related

- [Developing on tOS](/tos/developing) and [Rust](/tos/dev-rust).
- [Packages](/tos/packages) and [tpkg commands](/tos/tpkg-commands).
