title: Shell commands
lede: The commands built into the tOS shell. Run `help` for the list on your system and `man <command>` for details.
---
Commands that packages bring (such as `git`, `gcc` or `python3`) are on the [package list](/tos/package-list).

## Files and folders

| Command | What it does |
|---|---|
| `ls`, `cd`, `pwd` | List, change and show the current folder |
| `mkdir`, `rmdir`, `touch` | Make a folder, remove an empty folder, make an empty file |
| `cat`, `head`, `tail`, `hexdump` | Show a file, its first or last lines, or a hex dump |
| `cp`, `mv`, `rm` | Copy, move or rename, and remove files |
| `find` | Find files in a folder tree |
| `stat` | Size, mode and owner of a file |
| `edit` | A simple line editor |
| `tar`, `zip`, `unzip` | Make and unpack archives |
| `chmod`, `chown`, `chgrp`, `umask` | Permissions and owners |

## Text

| Command | What it does |
|---|---|
| `echo`, `tee` | Print text, and write the input to a file |
| `grep` | Search for a pattern in a file |
| `sort`, `uniq`, `rev` | Sort lines, drop repeated lines, reverse each line |
| `wc` | Count lines, words and characters |
| `yes`, `seq` | Repeat text, count numbers |
| `basename`, `dirname` | Parts of a path |

## System

| Command | What it does |
|---|---|
| `help`, `man` | Short help, and the manual |
| `version`, `about`, `uname` | The tOS version and system information |
| `date`, `cal`, `uptime`, `hostname` | Time, calendar, how long it has been running, the machine's name |
| `free`, `df`, `dmesg` | Memory use, disk use, the kernel log |
| `ps`, `htop`, `kill`, `log` | Processes, a live monitor, ending a process, tasks and the operation log |
| `env`, `which`, `alias`, `unalias`, `history` | The environment, where a command is, shortcuts, past commands |
| `clear`, `font`, `sleep` | Clear the screen, change the terminal font, wait |
| `reboot`, `shutdown`, `bootrec` | Restart, power off, restart into tOS Recovery |

## Users

| Command | What it does |
|---|---|
| `whoami`, `id`, `groups` | Who you are |
| `su`, `sudo`, `exit`, `logout` | Become another user, run as root, leave |
| `passwd`, `useradd`, `userdel` | Passwords and accounts |

## Network

| Command | What it does |
|---|---|
| `ping` | Ping a host |
| `ifconfig` | The network card, its addresses and counters |
| `wget` | Download a file over HTTP |
| `ws` | A WebSocket client: `ws wss://host/path [message]` |
| `tls` | `tls verify on\|off` turns certificate checking on or off; `tls trust <file>` trusts extra root certificates until restart |
| `ipsec` | Adds IPsec security associations (AH) between this machine and a peer |

## Disks, packages and programs

| Command | What it does |
|---|---|
| `disk` | List, mount, format and resize disks (see [Files, disks and users](/tos/files)) |
| `tpkg` | Install and manage packages (see [Packages](/tos/packages)) |
| `service` | `service list`, `service status <name>`, `service log <name>`: background services that packages bring |
| `notify` | Post a notification, or turn Do Not Disturb on and off |
| `open` | `open <app> [file]`: start an app (files, notepad, paint, diskutil, settings, ...) |
| `python`, `tsharp` | The built-in MicroPython and T# |
| `run` | Run a tOS program (`.t`) as a process |
