# NTFS driver test tools

`kernel/fs/ntfs.c` is compiled unchanged into a hosted test program, so it can be
exercised without QEMU and checked with sanitizers.

    # build (from this directory)
    gcc -O1 -g -fsanitize=address,undefined -I shim -I ../../kernel/fs ntfs_host.c -o ntfs_host

    # make a base image with real mkntfs, then fuzz it against an in-memory model
    truncate -s 64M base.img && mkfs.ntfs -F -Q base.img
    ./fuzzmany.sh 1 20 1200 base.img        # seeds 1..20, 1200 random ops each

    # grow / shrink
    truncate -s 128M base.img && ./ntfs_host base.img resize 0

`ntfscheck.py IMG [--tree]` is an independent checker (fixups, index order,
cluster ownership vs. `$Bitmap`, MFT bitmap, link counts). Run it on any image
the driver has written; it needs no code from the driver.
