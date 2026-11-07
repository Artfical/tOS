#!/system/pkg/busybox/bin/busybox sh
chk() { if [ "$1" = "$2" ]; then echo "gt: ok $3"; else echo "gt: FAIL $3 (got [$1] want [$2])"; fi; }
P=/system/pkg
export PATH=/system/pkg/gcc/bin:/system/pkg/binutils/bin:/system/pkg/make/bin:/system/pkg/bison/bin:/system/pkg/flex/bin:/system/pkg/m4/bin:/system/pkg/perl/bin:/system/pkg/autoconf/bin:/system/pkg/automake/bin:/system/pkg/libtool/bin:/system/pkg/cmake/bin:/system/pkg/ninja/bin:/system/pkg/pkgconf/bin:/system/pkg/coreutils/bin:/system/pkg/sed/bin:/system/pkg/grep/bin:/system/pkg/gawk/bin:/system/pkg/bash/bin:/system/pkg/busybox/bin:/usr/bin:/bin:$PATH:/usr/bin:/bin
cd /tmp; rm -rf at; mkdir -p at/src; cd at
cat > configure.ac <<'XX'
AC_INIT([hello], [1.0])
AM_INIT_AUTOMAKE([foreign -Wall])
AC_PROG_CC
LT_INIT
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
XX
cat > Makefile.am <<'XX'
lib_LTLIBRARIES = libhi.la
libhi_la_SOURCES = hi.c
bin_PROGRAMS = hello
hello_SOURCES = main.c
hello_LDADD = libhi.la
XX
printf 'const char *hi(void) { return "autotools-ok"; }\n' > hi.c
printf '#include <stdio.h>\nconst char *hi(void);\nint main(void) { puts(hi()); return 0; }\n' > main.c
export ACLOCAL_PATH=/system/pkg/libtool/share/aclocal
chk "$(autoconf --version | head -1)" "autoconf (GNU Autoconf) 2.72" "autoconf version"
chk "$(automake --version | head -1)" "automake (GNU automake) 1.17" "automake version"
chk "$(libtoolize --version | head -1 | cut -c1-17)" "libtoolize (GNU l" "libtoolize"
libtoolize --copy --force > lt.log 2>&1; echo "libtoolize rc=$?"
aclocal -I /system/pkg/libtool/share/aclocal > ac.log 2>&1; echo "aclocal rc=$?"; tail -2 ac.log
autoconf > ac2.log 2>&1; echo "autoconf rc=$?"; tail -2 ac2.log
automake --add-missing --copy > am.log 2>&1; echo "automake rc=$?"; tail -2 am.log
chk "$(test -x configure && echo yes)" yes "configure script generated"
./configure --prefix=/tmp/at/inst > cf.log 2>&1; echo "configure rc=$?"; tail -2 cf.log
make > mk.log 2>&1; echo "make rc=$?"; tail -3 mk.log
make install > in.log 2>&1; echo "install rc=$?"
chk "$(/tmp/at/inst/bin/hello)" autotools-ok "autoconf+automake+libtool project builds and runs"
