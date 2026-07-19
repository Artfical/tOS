#!/bin/bash
# tOS SDK Builder - Builds i386-elf cross-compiler + newlib for tOS
# Usage: ./build_sdk.sh [prefix]
# Default prefix: /usr/local/tos-sdk
#
# NOTE: tOS's ELF loader/exec support has been removed (see the
# "ELF Program Loading (removed)" section of README.md -- it let any
# user-run binary overwrite kernel memory, since this kernel maps all
# physical RAM as PTE_USER with no real kernel/user isolation). This
# script still builds a working cross-compiler, but binaries it
# produces can no longer be run on tOS until real address-space
# isolation exists.

set -e

PREFIX="${1:-/usr/local/tos-sdk}"
TARGET=i386-elf
CORES=$(nproc)

echo "=== tOS SDK Builder ==="
echo "Target: $TARGET"
echo "Prefix: $PREFIX"
echo "Cores:  $CORES"
echo ""

mkdir -p $PREFIX
export PATH="$PREFIX/bin:$PATH"

# ----- Download -----
download() {
    local url="$1"
    local file="$2"
    if [ ! -f "$file" ]; then
        echo "Downloading $file..."
        wget -q "$url" -O "$file" || curl -sL "$url" -o "$file"
    fi
}

echo "=== Step 1: Downloading sources ==="
mkdir -p /tmp/tos-sdk && cd /tmp/tos-sdk
download "https://ftp.gnu.org/gnu/binutils/binutils-2.43.tar.xz" "binutils-2.43.tar.xz"
download "https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz" "gcc-14.2.0.tar.xz"
download "https://sourceware.org/pub/newlib/newlib-4.4.0.20231231.tar.gz" "newlib-4.4.0.tar.gz"

# ----- Extract -----
echo "=== Step 2: Extracting ==="
for f in binutils-2.43.tar.xz gcc-14.2.0.tar.xz newlib-4.4.0.tar.gz; do
    [ -d "${f%%.tar*}" ] || tar xf "$f"
done

# GCC prerequisites
cd gcc-14.2.0
./contrib/download_prerequisites 2>&1 | tail -3 || true
cd ..

# Copy tOS newlib port into newlib source
TOS_PORT_SRC="$(dirname "$0")/programs/libgloss/tos"
TOS_NEWLIB_GLOSS="newlib-4.4.0/libgloss/tos"
mkdir -p "$TOS_NEWLIB_GLOSS"
cp "$TOS_PORT_SRC"/* "$TOS_NEWLIB_GLOSS/" 2>/dev/null || true

# ----- Build binutils -----
echo "=== Step 3: Building binutils ==="
mkdir -p build-binutils && cd build-binutils
../binutils-2.43/configure --target=$TARGET --prefix=$PREFIX \
    --disable-nls --disable-werror MAKEINFO=true 2>&1 | tail -3
make -j$CORES MAKEINFO=true 2>&1 | tail -5
make install MAKEINFO=true 2>&1 | tail -3
cd ..

# ----- Build GCC (stage 1) -----
echo "=== Step 4: Building GCC stage 1 ==="
mkdir -p build-gcc && cd build-gcc
../gcc-14.2.0/configure --target=$TARGET --prefix=$PREFIX \
    --disable-nls --enable-languages=c --without-headers \
    --disable-bootstrap --disable-libssp --disable-libgomp \
    --disable-libquadmath --disable-threads --disable-shared \
    --with-newlib MAKEINFO=true 2>&1 | tail -3
make -j$CORES all-gcc MAKEINFO=true 2>&1 | tail -5
make install-gcc MAKEINFO=true 2>&1 | tail -3
cd ..

# ----- Build newlib -----
echo "=== Step 5: Building newlib ==="
mkdir -p build-newlib && cd build-newlib
../newlib-4.4.0/configure --target=$TARGET --prefix=$PREFIX \
    --disable-nls MAKEINFO=true 2>&1 | tail -3
make -j$CORES MAKEINFO=true 2>&1 | tail -5
make install MAKEINFO=true 2>&1 | tail -3
cd ..

# ----- Build GCC (stage 2 with newlib) -----
echo "=== Step 6: Building GCC stage 2 (with newlib) ==="
mkdir -p build-gcc2 && cd build-gcc2
../gcc-14.2.0/configure --target=$TARGET --prefix=$PREFIX \
    --disable-nls --enable-languages=c --with-newlib \
    --disable-bootstrap --disable-libssp --disable-libgomp \
    --disable-libquadmath --disable-threads --disable-shared \
    MAKEINFO=true 2>&1 | tail -3
make -j$CORES MAKEINFO=true 2>&1 | tail -5
make install MAKEINFO=true 2>&1 | tail -3
cd ..

# ----- Build libtos for tOS -----
echo "=== Step 7: Building libtos ==="
cd "$TOS_PORT_SRC"
make CC=$TARGET-gcc AR=$TARGET-ar 2>&1 | tail -3
cp libtos.a crt0.o "$PREFIX/$TARGET/lib/"
cd /tmp/tos-sdk

echo ""
echo "=== SDK Build Complete ==="
echo "i386-elf-gcc: $PREFIX/bin/i386-elf-gcc"
echo ""
echo "To compile a tOS program:"
echo "  \$TARGET-gcc -m32 -ffreestanding -nostdlib -T program.ld \\"
echo "      crt0.o -ltos -lgcc -o hello.elf hello.c"
