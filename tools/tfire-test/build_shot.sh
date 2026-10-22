#!/bin/sh
# Builds the screenshot tool (shot_host.c): tFire's engine with tOS's fonts and drawing code, on the host: /tmp/tfire_shot
set -e
cd "$(dirname "$0")/../.."
sh tools/tfire-test/build_page.sh
N=third_party/netsurf
Q=third_party/quickjs
O=${TFIRE_HOST_OBJ:-/tmp/tfire_hostobj32}
for f in gfx png jpeg imgload ui_assets; do
    [ $O/d_$f.o -nt kernel/display/$f.c ] || gcc -m32 -isystem /tmp/tfire_inc32 -O2 -w -fno-builtin -Ikernel/display -Ikernel/lib -c kernel/display/$f.c -o $O/d_$f.o
done
if [ ! -f $O/libwebp.a ]; then
    for f in third_party/libwebp/src/dec/*.c third_party/libwebp/src/utils/*.c third_party/libwebp/src/dsp/*.c; do
        gcc -m32 -isystem /tmp/tfire_inc32 -O2 -w -Ithird_party/libwebp -c $f -o $O/webp_$(basename $f .c).o
    done
    ar rcs $O/libwebp.a $O/webp_*.o
fi
INC="-I$N/libparserutils/include -I$N/libwapcaplet/include -I$N/libhubbub/include -I$N/libcss/include -I$Q -Ikernel/tfire -Ikernel/net -Ikernel/display -Itools/tfire-test -Ithird_party/nanosvg -Ithird_party/libwebp"
gcc -m32 -isystem /tmp/tfire_inc32 -O1 -g -w -std=gnu99 -DTFIRE_HOST $INC \
    kernel/tfire/dom.c kernel/tfire/style.c kernel/tfire/anim.c kernel/tfire/layout.c kernel/tfire/engine.c kernel/tfire/js.c kernel/tfire/images.c \
    tools/tfire-test/shot_host.c $O/d_gfx.o $O/d_png.o $O/d_jpeg.o $O/d_imgload.o $O/d_ui_assets.o \
    $O/prelude.o $O/libqjs.a $O/libns.a $O/libwebp.a -lm -lpthread -o /tmp/tfire_shot
