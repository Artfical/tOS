# A real terminal over SSH, and xz

## terminal | SSH
- Logging in to tOS over SSH now gives a real terminal: the prompt, line editing, ^C that stops the running command and leaves the shell, the window size, and what a terminal does with output. sshd (the openssh package, sudo service start sshd) needs this update.
- Programs can open pseudo-terminals (/dev/ptmx and /dev/pts), and pass descriptors to each other over a socketpair, which is what SSH servers, terminal programs and script do.
- The BusyBox package is now a 64-bit build (BusyBox 1.36.1-3): its shell handles signals properly and runs its own commands.

## terminal | xz
- New xz package (sudo tpkg install xz): xz, unxz and xzcat for .xz files, with threads. Checked by packing and unpacking a 3.7 MB program and running it again.

## terminal | Files
- Programs can seek past the end of a file and write there (the gap reads as zeros), and truncate and ftruncate work. xz and other programs that write sparse files wrote wrong data before; scp no longer complains about truncate.

## settings | Stability
- When a program crashes, the log now shows its full registers, which makes the next bug quicker to find.
