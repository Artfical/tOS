# Node.js 24 and 22, and programs that start programs

## terminal | Node.js 24 and 22
- tOS now runs 64-bit Linux programs, such as the official Node.js builds. Install one with sudo tpkg install node24 or sudo tpkg install node22; each comes with its own npm (11 and 10).
- If another Node is installed already, tpkg tells you that node, npm and npx now belong to the new package, and gives them back when you remove it.
- Programs can start other programs: fork, exec, wait, pipes, signals and sockets between processes work, so Node's child_process does too.
- Paths and file names can be up to 255 characters long, which npm 11 needs.
- The base system carries the GNU C Library and the GCC runtime (licences in /lib/x86_64-linux-gnu) for these programs.
