title: Terminal and shell
lede: The Terminal app runs tOS's shell: a set of built-in commands, plus every command that an installed package brings.
---
## Using the shell

![The Terminal](shot:terminal.png)

Open **Terminal** in the dock. Type a command and press Enter. Tab completes command and file names, the up and down keys walk through the history, and `Ctrl C` stops a running program. The prompt shows who you are: a normal user, or `root`.

`help` lists the built-in commands, and `man` is the manual: `man` alone lists the pages, `man <command>` shows one.

## Built-in commands

The shell has dozens of built-in commands. The ones you will use first:

- **Files:** `ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `stat`, `find`, `edit`, `tar`, `zip`, `unzip`.
- **Text:** `echo`, `head`, `tail`, `wc`, `sort`, `uniq`, `grep`, `rev`, `tee`, `hexdump`.
- **System:** `version`, `about`, `uname`, `date`, `uptime`, `free`, `df`, `ps`, `htop`, `dmesg`, `kill`, `reboot`, `shutdown`.
- **Network:** `ping`, `ifconfig`, `wget`.
- **Packages:** `tpkg`. See [Packages](/tos/packages).

The full list is on [Shell commands](/tos/commands).

## Programs from packages

When you install a package, the commands it brings become available by name: after `sudo tpkg install git`, `git` just works. `tpkg which <command>` tells you which package a command comes from, and `which <command>` shows where it is.

If a package brings a command that already exists (for example `coreutils` brings `ls`, which the shell and busybox already have), tpkg warns you before it takes the name over.

## Users and `sudo`

![The password dialog for sudo, with the asking app on the padlock](shot:password.png)

tOS has Linux-style accounts. The first user you create joins the `sudo` group.

```
whoami
sudo tpkg install nano
su -
```

`sudo` asks for your password and remembers it for 5 minutes. In the live environment the user is `tos` and the password is empty. Things that change the system, such as `tpkg install`, need root.

## Scripts and languages

- `python` starts the MicroPython interpreter that is built in. After `tpkg install python3` you also have CPython.
- `tsharp` runs T#, tOS's own small language.
- `run` starts a tOS program (`.t`) as a process of its own.
- The `busybox` package gives you `sh` and the usual small tools; the `bash` package gives you bash.

## Working with Linux programs

Linux programs run from the Terminal like any other command. They get a real terminal: window size changes reach them, `Ctrl C` stops them, and full-screen programs such as vim, tmux and htop work. In an SSH session, `Ctrl Z` and job control (`fg`, `bg`, `jobs`) work too.

## Customise

`alias` sets shortcuts, `font` lists and changes the terminal font, `umask` sets the default permissions of new files.
