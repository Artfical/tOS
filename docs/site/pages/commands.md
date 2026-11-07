title: Shell commands
lede: Every command built into the tOS shell, with what it does and examples. Run `help` for the list on your system and `man <command>` for details.
---
Commands that packages bring (such as `git`, `gcc` or `python3`) are on the [package list](/tos/package-list). Commands marked **root** need `sudo`.

## Files and folders

| Command | What it does |
|---|---|
| `ls [path]` | List a folder |
| `cd <path>` | Change the current folder |
| `pwd` | Show the current folder |
| `mkdir <path>` | Make a folder |
| `rmdir <path>` | Remove an empty folder |
| `touch <path>` | Make an empty file, or set a file's time to now |
| `cat <path>` | Show a file |
| `head [-n N] <path>`, `tail [-n N] <path>` | The first or last N lines of a file |
| `hexdump <path>` | A hex dump of a file |
| `cp <src> <dst>` | Copy a file |
| `mv <src> <dst>` | Move or rename |
| `rm <path>` | Remove a file |
| `find <path>` | Find files in a folder tree |
| `stat <path>` | A file's size, mode and owner |
| `edit <path>` | A simple line editor |
| `tar c\|x\|t <archive> ...` | Make, unpack or list a tar archive |
| `zip <archive> <file...>` | Make a zip archive |
| `unzip <archive> [dest]` | Unpack a zip archive |
| `chmod <mode> <path>`, `chown`, `chgrp`, `umask` | Permissions and owners |

```
ls /system/pkg
mkdir work && cd work
touch a.txt
cp a.txt b.txt
tar c backup.tar work
```

## Text

| Command | What it does |
|---|---|
| `echo` | Print arguments |
| `tee <path>` | Write the input to the screen and to a file |
| `grep <pattern> <path>...` | Search files for a pattern |
| `sort <path>` | Sort lines |
| `uniq <path>` | Drop repeated adjacent lines |
| `rev <path>` | Reverse each line |
| `wc <path>` | Count lines, words and characters |
| `yes [str]` | Repeat a string |
| `seq [start] [step] <end>` | Print numbers |
| `basename <path>`, `dirname <path>` | Parts of a path |

## System

| Command | What it does |
|---|---|
| `help` | Short help for every command |
| `man [command]` | The manual: without an argument, the list of pages |
| `version` | The tOS build |
| `about` | About tOS |
| `uname` | System information |
| `date`, `cal` | The date and time, and a calendar |
| `uptime` | How long the system has been running |
| `hostname` | The machine's name |
| `whoami` | The current user |
| `free` | Memory use |
| `df` | File system use |
| `dmesg` | The kernel log |
| `log` | Running tasks and the operation log (disk operations, hex dumps) |
| `ps` | List processes |
| `htop` | A live process monitor |
| `kill <pid>` | End a process |
| `env` | Show the environment |
| `which <cmd>` | Where a command is |
| `alias [name=value]`, `unalias <name>` | Shortcuts |
| `history` | Past commands |
| `clear` | Clear the screen |
| `font` | List and change the terminal font |
| `sleep <seconds>` | Wait |
| `reboot`, `shutdown` | Restart, power off |
| `bootrec [-n]` | Restart into tOS Recovery (`-n`: only at the next start) |
| `exit`, `logout` | Leave the shell |

## Users (see [Files, disks and users](/tos/filesystems))

| Command | What it does |
|---|---|
| `id`, `groups` | User and group IDs |
| `su`, `sudo` | Become another user, or run as root |
| `passwd` | Change a password |
| `useradd`, `userdel` | Add and remove accounts (root) |

## Packages and services

| Command | What it does |
|---|---|
| `tpkg look`, `search`, `info`, `list`, `which` | Find and look at packages |
| `tpkg install`, `remove`, `update`, `upgrade` | Change what is installed (root) |
| `service list`, `service status <name>`, `service log <name>` | Background services that packages bring |

## Disks

| Command | What it does |
|---|---|
| `disk list` | The disks found |
| `disk info <device>` | Details of one |
| `disk mount <device> <dir> <type>`, `disk umount <dir>` | Mount and unmount |
| `disk format <device> <type>` | Format (erases it) |
| `disk resize <device> [size_mb]` | Grow a file system (root, unmounted) |

## Network

See [Network tools](/tos/network-tools) for all of these: `ping`, `ping6`, `ifconfig`, `ip6addr`, `wget`, `ws`, `tls`, `route`, `policy`, `fw`, `vlan`, `bridge`, `bond`, `gre`, `ipip`, `wg`, `ipsec`, `sctp_*`, `dccp_*`, `udplite_send`, `ipx`.

## Programs, scripts and apps

| Command | What it does |
|---|---|
| `python [file]` | The built-in MicroPython: REPL, or run a file |
| `tsharp [file]` | The built-in T# |
| `run <file.t>` | Run a tOS program as a process |
| `open <app> [file]` | Start an app (files, notepad, paint, diskutil, settings, mediaplayer, netmon, snake, 2048, ...) |
| `notify` | Post a notification, or turn Do Not Disturb on and off (see [Notifications](/tos/notifications)) |

## Sound, graphics and demos

| Command | What it does |
|---|---|
| `beep [on\|off\|test]` | System sounds on or off, or play a test click |
| `soundinfo` | Which audio backend is in use |
| `doom`, `wolf3d` | The games |
| `3d` | The rotating 3D cube |
| `vgatest` | A pixel graphics test |
| `crash` | The crash screen, for show |

See [Games and demos](/tos/games) and [Audio](/tos/audio).

## Related

- [Terminal and shell](/tos/terminal): how to use the shell.
- [Packages](/tos/packages): the commands that packages bring.
