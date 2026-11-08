# Go and Rust

## terminal | Programs (tpkg install ...)
- Go 1.27 (`sudo tpkg install go`): the compiler, the go command and the whole standard library. `go build`, `go run`, `gofmt` and HTTPS programs work; the package carries a ready-made build cache so a first build takes seconds.
- Rust 1.99 (`sudo tpkg install rust`): rustc, cargo and rustdoc; it installs the gcc package, which rustc links with. Programs with threads compile, link and run.

## terminal | Fixed
- Linux programs that start threads on memory they have not touched yet (Go's thread stacks) no longer fail with "cannot create thread".
- Signal handlers run on the alternate stack a thread sets with sigaltstack (SA_ONSTACK), per thread, as Go and other runtimes need.
- waitid() is available (Go waits for programs this way), including WNOWAIT.
- Files mapped shared and writable (Go's linker writes its output this way) are written back to the file on munmap, msync and exit; files mapped shared for reading no longer run out of the few shared segments (Go's compiler maps every package file).
