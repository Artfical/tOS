title: Terminal and shell
lede: The Terminal app runs tOS's shell: dozens of built-in commands, plus every command that an installed package brings. This page shows how to work in it.
icon: terminal
---
![The Terminal](shot:terminal.png)

## Opening the Terminal

Click **Terminal** in the dock, or choose it in the Launcher. A window opens with a prompt that shows who you are: a normal user, or `root`. You can open more than one Terminal.

## Typing commands

You type a command and press Enter. The Terminal helps:

| Key | What it does |
|---|---|
| **Tab** | Completes a command or a file name |
| **Up**, **Down** | Walk through the commands you typed before |
| **Ctrl+C** | Stops the program that is running |

`clear` clears the screen.

`history` lists past commands. `alias ll='ls -l'` sets a shortcut (`unalias` removes it). `env` shows the environment, and `which <name>` shows where a command is.

## Getting help

- `help` lists the built-in commands, one line each.
- `man` lists the manual pages, and `man <command>` shows one in full.
- `version` shows the build, `about` shows information about tOS, and `uname` shows the system.

## Built-in commands

The shell has dozens of built-in commands. The ones you will use first:

| Group | Commands |
|---|---|
| Files | `ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `stat`, `find`, `edit`, `tar`, `zip`, `unzip` |
| Text | `echo`, `head`, `tail`, `wc`, `sort`, `uniq`, `grep`, `rev`, `tee`, `hexdump` |
| System | `version`, `about`, `uname`, `date`, `uptime`, `free`, `df`, `ps`, `htop`, `dmesg`, `kill`, `reboot`, `shutdown` |
| Network | `ping`, `ifconfig`, `wget` |
| Packages | `tpkg` |

The full list, with what each command does, is on [Shell commands](/tos/commands).

## Files and folders

```
pwd                      # where am I
ls                       # what is here
ls /system/pkg           # what is in another folder
cd /tmp                  # go there
mkdir work               # make a folder
cd work
touch notes.txt          # make an empty file
edit notes.txt           # a simple line editor (or open Notepad)
cat notes.txt            # show it
cp notes.txt copy.txt    # copy
mv copy.txt old.txt      # rename or move
rm old.txt               # delete
```

`cd ..` goes up one level. `find <folder>` lists a whole tree.

## Programs from packages

When you install a package, the commands it brings become available **by name**. After `sudo tpkg install git`, you type `git`. `tpkg which <command>` tells you which package a command comes from, and `which <command>` shows where it is.

If a package brings a command that already exists (for example `coreutils` brings its own `ls`, which the shell and busybox have too), tpkg tells you before it takes the name over. The package's version is used from then on; removing the package gives the name back.

Programs you build yourself run by path: `./hello`.

## Users and `sudo`

tOS has Linux-style accounts. The first user you create belongs to the `sudo` group.

```
whoami
id
sudo tpkg install nano       # run one command as root
su -                         # become root for a while
exit                         # leave it again
```

`sudo` asks for your password and remembers it for 5 minutes. In the live environment the user is `tos` and the password is empty. Everything that changes the system, such as `tpkg install` and `useradd`, needs root. The password dialog looks like this, with the app that asks on the padlock:

![The sudo password dialog](shot:password.png)

## Scripts and languages

- `python` starts the built-in **MicroPython** interpreter. After `sudo tpkg install python3` you also have CPython (3.14 with pip) as `python3`.
- `tsharp` runs **T#**, tOS's own small language.
- `run <file.t>` starts a tOS program as a process of its own.
- The built-in shell takes one command at a time. For scripts with pipes, redirection, variables and loops, start a full shell: `sh` (from the `busybox` package) or `bash` (the `bash` package). Scripts that begin with `#!/bin/sh` or `#!/bin/bash` run directly.

See [Shell scripting](/tos/dev-shell) and [MicroPython and T#](/tos/scripting).

## Linux programs in the Terminal

Linux programs run from the Terminal like any other command. They get a real terminal: window size changes reach them, `Ctrl+C` stops them, and full-screen programs such as vim, tmux, htop and less work. See [Linux programs](/tos/linux-programs).

### Stopping a program and coming back (job control)

Press `Ctrl+Z` while a Linux program runs. The program stops, the shell tells you `[1]+  Stopped  busybox`, and the prompt returns. Then:

| Command | What it does |
|---|---|
| `jobs` | Lists the stopped programs |
| `fg` (or `fg 2`) | Brings the latest (or number 2) back to the front |
| `bg` | Lets it run in the background |

`Ctrl+C` ends the running program even when it never reads the keyboard (a `sleep`, for example). The same works in an SSH session.

![Ctrl+Z, jobs, fg and Ctrl+C in the Terminal](shot:terminal_jobs.png)

## Processes

`ps` lists the processes, `htop` is a live monitor, and `kill <pid>` ends one. The **Activity** app shows the same in a window: see [Activity, Network and Disk Utility](/tos/system-apps).

## Customise

| Command | What it does |
|---|---|
| `alias`, `unalias` | Shortcuts for commands |
| `font` | Lists and changes the Terminal font |
| `umask` | Sets the default permissions of new files |
| `hostname` | Shows the machine's name |
| `beep on`, `beep off` | System sounds |

## Logging and the serial port

What the kernel logs is shown by `dmesg` and `log`. In a virtual machine, start QEMU with `-serial file:serial.log` to keep the kernel's log in a file as well.

## Related

- [Shell commands](/tos/commands): every built-in command.
- [Packages](/tos/packages): `tpkg` in depth.
- [Files, disks and users](/tos/filesystems): permissions and accounts.
