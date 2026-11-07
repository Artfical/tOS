title: Files, disks and users
lede: How tOS keeps files, which disk formats it reads and writes, and how accounts and permissions work.
---
## The file system

tOS starts with two file systems:

- **ramfs** at `/`: the root, held in memory. What is only in memory is gone after a restart.
- **tFS** at `/mnt`: tOS's own on-disk file system (4 KB blocks, a bitmap for free space, small files stored in the inode), mounted when a disk with tFS is found.

Folders you will meet: `/system` (system files, and `/system/pkg` where packages live), `/tmp`, `/etc`, and `/Volumes` for removable disks.

## Disk file systems

![Disk Utility: First Aid](shot:diskutil_firstaid.png)

The `disk` command and the Disk Utility app can look at, mount, format and grow disks of these types, with full read and write support:

| File system | Notes |
|---|---|
| FAT16, FAT32, exFAT | USB sticks and memory cards |
| ext2, ext3, ext4 | Linux disks (ext3 journal, ext4 extents) |
| NTFS | Windows disks |
| Btrfs | Linux, with checksums |
| XFS | Version 5 volumes. `disk format` makes a Linux-compatible volume |

```
disk list                          # the disks tOS found
disk mount <device> <dir> <type>   # mount one
disk format <device> <type>        # format one (this erases it)
disk resize <device> [size_mb]     # grow a file system (unmounted, root only)
```

Resizing grows ext2/3/4, Btrfs, NTFS, FAT and exFAT; shrinking is not implemented. FAT and exFAT can only grow as far as their allocation table has room, which volumes formatted by tOS leave plenty of.

Disks are found on IDE, AHCI, NVMe and USB. A USB disk is mounted by itself at `/Volumes/<label>`.

## Users and groups

tOS has Linux-style accounts in `/etc/passwd`, `/etc/shadow` (passwords hashed with PBKDF2-HMAC-SHA256, readable only by root) and `/etc/group`. The installer asks for the computer name and the first user, who joins the `sudo` group.

| Command | What it does |
|---|---|
| `whoami`, `id`, `groups` | Who you are |
| `su`, `sudo` | Become another user, or run one command as root. `sudo` needs the `sudo` group and remembers the password for 5 minutes |
| `passwd` | Change a password |
| `useradd`, `userdel` | Add and remove accounts (root) |
| `chmod`, `chown`, `chgrp`, `umask`, `stat` | Permissions and owners |

## Permissions

Files have owner, group and other bits (read, write, execute), and a folder needs its execute bit to be entered. The sticky bit and setuid/setgid work as on Linux. These are checked on the file systems that store owners: ramfs, tFS, ext2/3/4, XFS and Btrfs. FAT, exFAT and NTFS have no owners and are not checked.

Changing or deleting anything under `/system` needs root and the password, once per command.
