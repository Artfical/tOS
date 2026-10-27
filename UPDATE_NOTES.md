# npm install works; BusyBox from tpkg

## terminal | Terminal and tpkg
- `npm install` now works: npm downloads, checks and installs packages from the npm registry (update Node with `sudo tpkg upgrade`).
- `sudo tpkg install busybox` adds 92 Unix tools: grep, sed, awk, vi, tar, gzip, diff, find, wget, xxd and more.
- When a package replaces commands tOS already has (BusyBox's ls, cat, mkdir...), tpkg lists them and says how to undo it: `sudo tpkg remove busybox` brings tOS's own back. cd, sudo, su and tpkg always stay tOS's.
- Programs installed on the disk start in seconds instead of minutes.
- `tpkg` says why it can't reach the server when the network is down.

## files | Files and folders
- Moving and renaming between folders works on the startup disk, and renaming over an existing file replaces it, the way Linux programs expect.
- `touch` creates the file when it isn't there.
