# Node.js and npm, from tpkg

## terminal | Terminal and tpkg
- Install Node.js 8 and npm with `sudo tpkg install node`; `node`, `npm` and `npx` are then commands in Terminal.
- `npm install` does not finish yet; Node itself runs scripts, reads and writes files, and uses the network.
- tpkg now gets packages from update.artfical.com, checks each download's SHA-256, carries on after a dropped connection, and installs to /system/pkg (on the disk when tOS is installed).
- The packages, their licenses and their sources are listed at update.artfical.com/pkg.

## settings | Linux programs
- tOS runs static 32-bit Linux programs: threads, timers, files and pipes, /dev/urandom, and network connections over tOS's own TCP/IP (DNS, HTTP and HTTPS).
- Terminal starts them, and scripts that begin with `#!/usr/bin/env node`, with the session's HOME, USER, PWD and PATH.
- The initrd can hold up to 8,192 files and folders, with long paths and symbolic links.
