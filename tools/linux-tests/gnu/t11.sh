#!/system/pkg/busybox/bin/busybox sh
# Rust: rustc compiles and links a program (with the gcc package's cc); threads and the standard library work
[ -x /system/pkg/rust/bin/rustc-tos ] || exit 0
chk() { if [ "$1" = "$2" ]; then echo "rust: ok $3"; else echo "rust: FAIL $3 (got [$1] want [$2])"; fi; }
export HOME=/root PATH=/usr/bin:/bin:/system/pkg/busybox/bin
chk "$(rustc --version | cut -d' ' -f1-2)" "rustc 1.99.1" version
chk "$(cargo --version | cut -d' ' -f1-2)" "cargo 1.99.1" cargo-version
cd /tmp && cp /programs/gt/rs_hello.rs hello.rs && rustc hello.rs -o hello_rs
chk "$(./hello_rs | head -1)" "hello from Rust on tOS: sum=55" build-run
