#!/system/pkg/busybox/bin/busybox sh
# Wine: a Windows console program runs (only when the wine package is in the initrd)
[ -x /system/pkg/wine/bin/tos-wine ] || exit 0
chk() { if [ "$1" = "$2" ]; then echo "wine: ok $3"; else echo "wine: FAIL $3 (got [$1] want [$2])"; fi; }
export HOME=/root
chk "$(/system/pkg/wine/bin/wine --version)" "wine-10.0" version
chk "$(/system/pkg/wine/bin/tos-wine cmd /c echo hello-from-windows 2>/dev/null | tr -d '\r')" "hello-from-windows" cmd-echo
# an installer: msiexec puts the package's file in Program Files (x86), where a 32-bit package goes
/system/pkg/wine/bin/tos-wine msiexec /i /programs/gt/test.msi /qn >/dev/null 2>&1
chk "$(cat "$HOME/.wine/drive_c/Program Files (x86)/tOS Test/hello.txt" 2>/dev/null)" "hello from the installer" msi-install
