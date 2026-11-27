#!/bin/bash
# GTK 3 and what it stands on, built on the host for tOS (prefixes under /system/pkg, RPATHs to them).
#   build.sh STEP [STEP ...]     steps: protos xlibs libpng glib harfbuzz fribidi cairo pango pixbuf atk epoxy gtk3 icons
# Sources are kept in /root/gtk-build/src (they are shipped with the packages); a build folder is deleted after its step.
B=/root/gtk-build; P=/system/pkg
mkdir -p $B/src $B/work $B/logs
log() { echo "$(date +%T) $*" | tee -a $B/progress.log; }
GTK=$P/gtk3; XL=$P/xlibs; PNG=$P/libpng
PCP=$GTK/lib/pkgconfig:$GTK/share/pkgconfig:$XL/lib/pkgconfig:$XL/share/pkgconfig:$PNG/lib/pkgconfig
for d in xorg fontconfig expat pcre2 libffi zlib-dev xkbcommon xcb-util openssl-dev; do PCP=$PCP:$P/$d/lib/pkgconfig:$P/$d/share/pkgconfig; done
export PKG_CONFIG_PATH=$PCP
RP=""
for d in gtk3 xlibs libpng xorg fontconfig expat pcre2 libffi zlib xkbcommon xcb-util; do RP="$RP -Wl,-rpath,$P/$d/lib"; done
export LDFLAGS="$RP -L$GTK/lib -L$XL/lib -L$PNG/lib -L$P/xorg/lib"
export CFLAGS="-O2 -I$GTK/include -I$XL/include -I$PNG/include -I$P/xorg/include"
export CPPFLAGS="-I$GTK/include -I$XL/include -I$PNG/include -I$P/xorg/include"
export PATH=$GTK/bin:/root/.local/bin:$PATH
export LD_LIBRARY_PATH=$GTK/lib:$XL/lib:$PNG/lib:$P/xorg/lib:$P/fontconfig/lib:$P/expat/lib:$P/pcre2/lib:$P/libffi/lib

fetch() { # url [file]
    local f=$B/src/${2:-$(basename $1)}
    [ -s "$f" ] || wget -q -O "$f" "$1" || { log "FAIL download $1"; rm -f "$f"; return 1; }
    echo "$f"
}
unpack() { # tarball -> $B/work/<dir>
    rm -rf $B/work/$2; mkdir -p $B/work/$2
    tar -xf "$1" -C $B/work/$2 --strip-components=1 || return 1
}
done_step() { rm -rf $B/work/$1; df -h / | tail -1 | awk '{print "   disk free: " $4}' | tee -a $B/progress.log; log "done $1"; }
auto() { # name url prefix [configure args]
    local n=$1 u=$2 pre=$3; shift 3
    log "== $n"; local f; f=$(fetch $u) || return 1
    unpack $f $n || return 1
    ( cd $B/work/$n && ./configure --prefix=$pre "$@" > $B/logs/$n.conf.log 2>&1 && make -j5 > $B/logs/$n.make.log 2>&1 && make install > $B/logs/$n.inst.log 2>&1 ) \
        || { log "FAIL $n (see logs/$n.*.log)"; return 1; }
    done_step $n
}
mes() { # name url prefix [meson args]
    local n=$1 u=$2 pre=$3; shift 3
    log "== $n"; local f; f=$(fetch $u) || return 1
    unpack $f $n || return 1
    ( cd $B/work/$n && meson setup build --prefix=$pre --libdir=lib --buildtype=release --wrap-mode=nodownload "$@" > $B/logs/$n.conf.log 2>&1 \
      && ninja -C build -j5 > $B/logs/$n.make.log 2>&1 && ninja -C build install > $B/logs/$n.inst.log 2>&1 ) \
        || { log "FAIL $n (see logs/$n.*.log)"; return 1; }
    done_step $n
}

step() {
case $1 in
protos)
    auto xorgproto https://xorg.freedesktop.org/archive/individual/proto/xorgproto-2024.1.tar.xz $XL ;;
xlibs)
    # the X client libraries the xorg package does not have (libX11, libXext and libXrender are in it)
    for l in libXfixes-6.0.1 libXi-1.8.2 libXrandr-1.5.4 libXcursor-1.2.3 libXinerama-1.1.5 libXdamage-1.1.6 libXcomposite-0.4.6 libXtst-1.2.5; do
        auto ${l%-*} https://xorg.freedesktop.org/archive/individual/lib/$l.tar.xz $XL --disable-static --disable-docs --disable-specs || return 1
    done ;;
libpng)
    auto libpng https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz $PNG --disable-static ;;
glib)
    mes glib https://download.gnome.org/sources/glib/2.80/glib-2.80.5.tar.xz $GTK -Dtests=false -Dnls=disabled -Dlibmount=disabled -Dselinux=disabled \
        -Dxattr=false -Dlibelf=disabled -Dsysprof=disabled -Dman-pages=disabled -Ddocumentation=false -Dintrospection=disabled -Ddtrace=false -Dglib_debug=disabled ;;
harfbuzz)
    mes harfbuzz https://github.com/harfbuzz/harfbuzz/releases/download/9.0.0/harfbuzz-9.0.0.tar.xz $GTK -Dtests=disabled -Ddocs=disabled -Dintrospection=disabled \
        -Dcairo=disabled -Dicu=disabled -Dfreetype=enabled -Dglib=enabled -Dgobject=enabled ;;
fribidi)
    mes fribidi https://github.com/fribidi/fribidi/releases/download/v1.0.15/fribidi-1.0.15.tar.xz $GTK -Ddocs=false -Dtests=false -Dbin=false ;;
cairo)
    mes cairo https://cairographics.org/releases/cairo-1.18.2.tar.xz $GTK -Dtests=disabled -Dgtk_doc=false -Dxlib=enabled -Dxcb=enabled -Dfreetype=enabled \
        -Dfontconfig=enabled -Dpng=enabled -Dglib=enabled -Dzlib=enabled -Dspectre=disabled -Dsymbol-lookup=disabled ;;
pango)
    mes pango https://download.gnome.org/sources/pango/1.54/pango-1.54.0.tar.xz $GTK -Dintrospection=disabled -Dgtk_doc=false -Dbuild-testsuite=false -Dbuild-examples=false -Dfontconfig=enabled -Dcairo=enabled ;;
pixbuf)
    mes gdk-pixbuf https://download.gnome.org/sources/gdk-pixbuf/2.42/gdk-pixbuf-2.42.12.tar.xz $GTK -Dintrospection=disabled -Dman=false -Dgtk_doc=false -Dtests=false -Dinstalled_tests=false \
        -Dpng=enabled -Djpeg=disabled -Dtiff=disabled -Dothers=disabled -Dbuiltin_loaders=png,jpeg,gif,bmp -Drelocatable=false ;;
atk)
    mes atk https://download.gnome.org/sources/atk/2.38/atk-2.38.0.tar.xz $GTK -Dintrospection=false -Ddocs=false ;;
epoxy)
    mes libepoxy https://download.gnome.org/sources/libepoxy/1.5/libepoxy-1.5.10.tar.xz $GTK -Degl=no -Dglx=yes -Dx11=true -Dtests=false -Ddocs=false ;;
atkbridge)
    log "== atk-bridge (a stub: no D-Bus, no screen reader)"
    mkdir -p $GTK/lib/pkgconfig $GTK/include/at-spi2-atk/2.0 &&
    gcc -O2 -shared -fPIC -Wl,-soname,libatk-bridge-2.0.so.0 -o $GTK/lib/libatk-bridge-2.0.so.0 $B/stubs/atk-bridge.c &&
    ln -sf libatk-bridge-2.0.so.0 $GTK/lib/libatk-bridge-2.0.so && cp $B/stubs/atk-bridge.h $GTK/include/at-spi2-atk/2.0/ &&
    cat > $GTK/lib/pkgconfig/atk-bridge-2.0.pc <<PC
prefix=$GTK
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: atk-bridge-2.0
Description: the accessibility bridge (a stub in tOS: it does nothing)
Version: 2.38.0
Libs: -L\${libdir} -latk-bridge-2.0
Cflags: -I\${includedir}/at-spi2-atk/2.0
PC
    log "done atk-bridge" ;;
gtk3)
    mes gtk https://download.gnome.org/sources/gtk+/3.24/gtk+-3.24.43.tar.xz $GTK -Dx11_backend=true -Dwayland_backend=false -Dbroadway_backend=false -Dquartz_backend=false \
        -Dwin32_backend=false -Dintrospection=false -Dgtk_doc=false -Dman=false -Ddemos=false -Dexamples=false -Dtests=false -Dinstalled_tests=false -Dprint_backends=file \
        -Dcolord=no -Dcloudproviders=false -Dtracker3=false -Dxinerama=yes ;;
*) log "unknown step $1"; return 1 ;;
esac
}

for s in "$@"; do step $s || { log "STOPPED at $s"; exit 1; }; done
log "all steps done: $*"
