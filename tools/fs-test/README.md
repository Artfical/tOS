# Filesystem driver test harness

`fs_host.c` compiles one kernel filesystem driver unchanged into a hosted program (with
ASan/UBSan) and runs it against an image file.

    ./build.sh ext4 1            # FS and NAMEMODE (0 Unicode ci, 1 bytes cs, 2 8.3 ci, 3 ASCII ci)
    /tmp/fs_host_ext4 IMG fuzz SEED 300                    # random ops checked against an in-memory model
    /tmp/fs_host_ext4 IMG script mkdir:d create:d/f:50 ls:d cat:d/f rm:d/f mv:a:b

Check the result with the filesystem's own fsck (`e2fsck -fn`, `fsck.vfat -n`,
`fsck.exfat -n`) on the image afterwards.
