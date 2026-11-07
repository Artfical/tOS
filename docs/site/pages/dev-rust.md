title: Rust
lede: Rust is not yet packaged for tOS, but you can cross-compile or use a static binary. This page documents the current state and workarounds.
---
## Current status

Rust (`rustc`, `cargo`) is **not yet available as a tpkg package**. The Rust toolchain has complex bootstrap requirements (it builds itself) and depends on LLVM, which is not yet ported to tOS.

## Workarounds

### 1. Cross-compile from Linux

On a Linux host with `rustup`:

```bash
rustup target add x86_64-unknown-linux-musl
cargo build --target x86_64-unknown-linux-musl --release
```

Copy the resulting static binary to tOS and run it. It will use the musl libc (tOS uses glibc 2.35, but musl binaries work if fully static).

### 2. Use a pre-built static binary

Download a statically linked Rust binary from the upstream releases (e.g. from `rust-lang.org`) and run it on tOS. The `x86_64-unknown-linux-gnu` builds work if they are fully static or only depend on glibc 2.35+.

### 3. Use a container

On a Linux host with Docker/Podman:

```bash
podman run --rm -v $(pwd):/src -w /src rust:1.80 \
  cargo build --target x86_64-unknown-linux-musl --release
```

## What works on tOS once you have the binary

- Pure-Rust binaries (no C dependencies) run unmodified.
- Binaries that link to `libc` need glibc 2.35+ (tOS has 2.35).
- Networking (`std::net`, `tokio`, `async-std`) works through tOS's socket layer.
- Threads (`std::thread`, `tokio::task`) work through tOS's pthreads implementation.
- File I/O works through the VFS layer.

## What does not work yet

- `cargo` itself (needs Rust compiler).
- `rustc` (needs LLVM, which is not ported).
- Crates that need `cc` crate (C code compilation at build time).
- Proc-macros that need dynamic loading (`.so` plugins).

## Future plans

- Package `rustc` and `cargo` once LLVM is ported to tOS (tracking issue on GitHub).
- Provide a `rustup`-style installer that downloads pre-built toolchains.

## Current workaround for Rust developers

Develop on Linux/macOS, cross-compile with `--target x86_64-unknown-linux-musl`, test on tOS in QEMU:

```bash
qemu-system-x86_64 -cdrom tOS.iso -m 2048 -nic user,model=e1000 \
  -serial file:serial.log -display none
```

Copy your binary to the QEMU disk image or transfer via `scp`/`rsync` from the host.

## Related

- [Developing on tOS](/tos/developing)
- [Building tOS from source](/tos/build-tos)
EOF