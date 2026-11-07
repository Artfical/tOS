title: Package list
lede: Every package on the server, with its version and licence. Install one with `sudo tpkg install <name>`.
---
A package is offered to your system only when its build can run it, so the list on your system (`tpkg look`) can be shorter than this one until you have updated tOS. Packages marked `-dev` hold library headers and link files, and need `gcc`.

{{packages}}
