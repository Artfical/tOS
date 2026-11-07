#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/system/pkg/gcc/bin:/system/pkg/binutils/bin:/system/pkg/make/bin:/system/pkg/bison/bin:/system/pkg/flex/bin:/system/pkg/m4/bin:/system/pkg/perl/bin:/system/pkg/autoconf/bin:/system/pkg/automake/bin:/system/pkg/libtool/bin:/system/pkg/cmake/bin:/system/pkg/ninja/bin:/system/pkg/pkgconf/bin:/system/pkg/coreutils/bin:/system/pkg/sed/bin:/system/pkg/grep/bin:/system/pkg/gawk/bin:/system/pkg/bash/bin:/system/pkg/busybox/bin:/usr/bin:/bin:$PATH:/usr/bin:/bin
cd /tmp; rm -rf cm; mkdir -p cm/lib cm/app; cd cm
cat > CMakeLists.txt <<'XX'
cmake_minimum_required(VERSION 3.10)
project(demo C)
add_subdirectory(lib)
add_executable(app app/main.c)
target_link_libraries(app mylib)
find_package(ZLIB REQUIRED)
target_link_libraries(app ZLIB::ZLIB)
enable_testing()
add_test(NAME run COMMAND app)
XX
cat > lib/CMakeLists.txt <<'XX'
add_library(mylib STATIC my.c)
target_include_directories(mylib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
XX
printf 'int answer(void) { return 42; }\n' > lib/my.c; printf 'int answer(void);\n' > lib/my.h
cat > app/main.c <<'XX'
#include <stdio.h>
#include <zlib.h>
#include "my.h"
int main(void) { printf("answer=%d zlib=%s\n", answer(), zlibVersion()); return 0; }
XX
export CC=/system/pkg/gcc/bin/gcc CMAKE_PREFIX_PATH=/system/pkg/zlib-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DZLIB_ROOT=/system/pkg/zlib-dev > cm.log 2>&1; echo "cmake rc=$?"; tail -3 cm.log
ninja -C build > nj.log 2>&1; echo "ninja rc=$?"; tail -3 nj.log
chk "$(LD_LIBRARY_PATH=/system/pkg/zlib-dev/lib build/app | cut -c1-9)" answer=42 "cmake+ninja project built with gcc and linked with zlib"
(cd build && ctest 2>&1 | tail -3)
rm -rf build2; cmake -S . -B build2 -G "Unix Makefiles" > cm2.log 2>&1; echo "cmake makefiles rc=$?"; make -C build2 > mk.log 2>&1; echo "make rc=$?"
chk "$(test -x build2/app && echo built)" built "cmake Unix Makefiles generator with make"
