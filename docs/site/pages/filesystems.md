title: Files, disks and users
lede: How tOS lays out its files, which disk formats it reads and writes, how permissions work, and how accounts are managed.
---
## The layout

tOS has one tree of folders, starting at `/`. Disks and USB sticks are attached to it as folders.

| Folder | What is in it |
|---|---|
| `/system` | System files. `/system/pkg/<name>` holds each installed package |
| `/tmp` | Temporary files |
| `/etc` | Configuration: `passwd`, `shadow`, `group` and more |
| `/home/<user>` | A user's own files |
| `/Volumes/<label>` | A USB disk or other removable disk that you plug in |
| `/mnt` | The tFS disk, when the computer has one |
| `/proc` | Files that show the state of the system and of processes |
| `/dev` | Devices: the terminals, `null`, `zero`, `urandom`, the window devices |
| `/bin`, `/usr/bin` | The commands of the installed packages, by name |

## The file systems

tOS starts with two file systems:

- **ramfs**, at `/`: the root, held in memory. What is only in memory is gone after a restart.
- **tFS**, at `/mnt`: tOS's own on-disk file system. It has 4 KB blocks, a bitmap for free space, small files stored inside the inode, and indirect and double-indirect blocks for big files. It is mounted when a disk with tFS is found. The source is at [git.artfical.com/Artfical/tfs](https://git.artfical.com/Artfical/tfs).

The root file system and your packages live in memory, so a machine that installs a lot needs enough of it (give a virtual machine `-m 2048` or more).

## Disk file systems

The `disk` command and Disk Utility can look at, mount, format and grow disks of these types, with full read and write support:

| File system | Used for |
|---|---|
| FAT16, FAT32 | USB sticks, memory cards, older systems |
| exFAT | Big USB sticks and memory cards |
| ext2, ext3, ext4 | Linux disks (ext3 is journaled, ext4 uses extents) |
| NTFS | Windows disks |
| Btrfs | Linux, with checksums |
| XFS | Linux; version 5 volumes. `disk format` makes a Linux-compatible volume |

Disks are found on IDE, AHCI, NVMe and USB.

```
disk list                          # the disks tOS found
disk info <device>                 # details of one
disk mount <device> <dir> <type>   # mount it
disk umount <dir>                  # unmount it
disk format <device> <type>        # format it (this erases it)
disk resize <device> [size_mb]     # grow a file system (unmounted, root only)
```

The graphical way is **Disk Utility**: see [Activity, Network and Disk Utility](/tos/system-apps). A USB disk that you plug in is mounted by itself at `/Volumes/<label>`: see [Notifications and USB disks](/tos/notifications).

### Growing a file system

`disk resize` grows a file system to the given size, or to the whole device when you give no size. It grows ext2, ext3 and ext4, Btrfs, NTFS, FAT16/32 and exFAT. Shrinking is not implemented. FAT and exFAT keep their allocation table in front of the data, so they can only grow as far as the table already has room: volumes formatted by tOS leave room for several times their size, volumes from other tools may not.

## Users and groups

tOS has Linux-style accounts, kept in `/etc/passwd`, `/etc/shadow` (passwords hashed with PBKDF2-HMAC-SHA256, readable by root only, mode 0600) and `/etc/group`. The installer asks for the first user, who joins the `sudo` group.

| Command | What it does |
|---|---|
| `whoami`, `id`, `groups` | Who you are, and which groups you are in |
| `su`, `sudo` | Become another user, or run one command as root |
| `passwd` | Change a password |
| `useradd`, `userdel` | Add and remove accounts (root only) |
| `chmod`, `chown`, `chgrp` | Change a file's mode, owner and group |
| `umask` | The default mode of new files |
| `stat` | A file's size, mode and owner |

`sudo` is for root and for the members of the `sudo` (or `wheel`) group, and the password is remembered for 5 minutes.

## Permissions

Every file and folder has an **owner**, a **group** and **mode bits** for the owner, the group and everyone else: read (`r`), write (`w`) and execute (`x`). A folder needs `x` to be entered. The **sticky bit** (as on `/tmp`) lets only the owner delete a file, and **setuid** and **setgid** programs run as their owner or group, but only if the file cannot be changed by anyone else.

These are checked on the file systems that store owners: ramfs, tFS, ext2/3/4, XFS and Btrfs. FAT, exFAT and NTFS have no owners, so they are not checked. Changing or deleting anything under `/system` needs root and the password, once per command.

```
ls -l          # shows the mode and owner of each file
chmod 755 run.sh
chmod +x run.sh
sudo chown alice notes.txt
```

## Links, times and open files

- **Symbolic links** work everywhere, including relative ones. `ls` shows the link, `rm` removes the link and not what it points to.
- Files keep their **modification time**; `touch` sets it to now and Linux programs such as `make`, `tar` and `rsync` use it.
- A file that is **deleted while a program has it open** stays usable until the program closes it.
- Several descriptors for one open file (from `dup` or `fork`) **share its position**.
- Files larger than 2 GB are not supported: programs get a clear error.

## Related

- [Notifications and USB disks](/tos/notifications) and [Files](/tos/files-app).
- [Security](/tos/security).
