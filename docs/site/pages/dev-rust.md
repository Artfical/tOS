title: Rust
lede: Rust 1.99 runs on tOS: rustc, cargo and the standard library, from a tpkg package. Compile and run Rust programs on tOS itself.
---
## Install

```
sudo tpkg install rust
rustc --version
cargo --version
```

The package is about 205 MB to download and about 600 MB on disk (tOS keeps files in memory, so give the machine 4 GB if you can). It brings `rustc`, `cargo` and `rustdoc`, and it installs the `gcc` package with it, because `rustc` links programs with the C compiler.

> **sudo:** `sudo tpkg install rust` (and `sudo tpkg remove rust`) change the system, so they need root. Compiling and running programs needs no root.

## Your first program

```
cat > hello.rs <<'EOF'
fn main() {
    let v: Vec<u32> = (1..=10).collect();
    println!("sum = {}", v.iter().sum::<u32>());
    let h = std::thread::spawn(|| 6 * 7);
    println!("thread says {}", h.join().unwrap());
}
EOF
rustc hello.rs -o hello
./hello
```

This prints `sum = 55` and `thread says 42`.

## The commands

| Command | What it does |
|---|---|
| `rustc` | The compiler. A wrapper starts the real one with the `gcc` package's compiler as the linker, named by its full path so that it finds its own files |
| `cargo` | The build tool. The wrapper points it at that `rustc`, that linker, and the CA bundle for HTTPS downloads |
| `rustdoc` | The documentation tool |

The real programs are in `/system/pkg/rust/bin/`.

## What was tested

These ran on tOS (in a QEMU virtual machine) and worked:

| Test | Result |
|---|---|
| `rustc --version` | rustc 1.99.1 |
| `cargo --version` | cargo 1.99.1 |
| Compile and link a program that uses collections, `println!` and `std::thread`, then run it | works |

Not tested yet: building a project with `cargo build`, downloading crates from the internet, `cargo test`, `rustdoc`, and programs that build C code through the `cc` crate. They may work; this page does not promise it.

## Size and speed

`rustc` is large: its compiler library and LLVM are about 370 MB, and tOS reads them into memory each time `rustc` starts. A first compile takes a while on a slow or emulated machine. On real hardware or a virtual machine with hardware acceleration it is much quicker.

## Related

- [Developing on tOS](/tos/developing) and [Go](/tos/dev-go).
- [C and C++](/tos/dev-c): the `gcc` package that Rust uses to link.
- [Packages](/tos/packages) and [tpkg commands](/tos/tpkg-commands).
