title: tpkg troubleshooting
lede: Every error message you can find in the client with its cause and fix, in a table plus explanations.
---
## Quick reference

| Error message | Cause | Fix |
|---|---|---|
| `server unreachable` | Network down, DNS, firewall | Check `ping update.artfical.com`, try `curl -I https://update.artfical.com` |
| `package not found` | Not in index for your build | `sudo tpkg update`, then `sudo tpkg update` after Software Update |
| `min_build 619 > current 618` | Your build is too old | Software Update, then `sudo tpkg update` |
| `needs root` | Tried install/remove/upgrade without sudo | Run with `sudo` |
| `signature verification failed` | Corrupted download or not from server | Retry; if persistent, report it |
| `needed by: <pkg>` | Another package depends on this | Remove the dependent first, or `--force` |
| `command conflict: <cmd>` | Another package owns this name | `--force` to overwrite, or remove the other |
| `disk full` | No space on ramfs | `df -h`, `sudo tpkg remove <unused>` |
| `not found` (remove) | Package not installed | Check `tpkg list` |
| `build too old` | Your build < package's min_build | Software Update |
| `not found` (search) | No match in index | Try broader search term |
| `download failed` | Network interrupted | Retry; check `ping update.artfical.com` |
| `could not save` (install) | Disk full or read-only | `df -h`, check mount options |
| `could not open that file` (info) | Package not installed | `tpkg list` to check |

## Detailed explanations

### `server unreachable`

The client cannot reach `update.artfical.com:443`. Check:

```bash
ping update.artfical.com
curl -I https://update.artfical.com
```

If the host resolves but the connection times out, a firewall may block port 443. If DNS fails, check `/etc/resolv.conf`.

### `min_build 619 > current 618`

Your tOS build is older than the package requires. The package was added after your build. Fix:

1. Run Software Update (in Settings or `sudo tpkg update` after update).
2. Reboot into the new build.
3. Run `sudo tpkg update` to refresh the index.

### `signature verification failed`

The downloaded `.tpkg` does not match the server's ECDSA signature. Causes:

- Download was truncated (network glitch).
- The file was modified after download (unlikely).
- The server's copy is corrupted (rare).

**Fix:** Retry `sudo tpkg install <name>`. If it persists, report it on GitHub with the package name and build number.

### `needed by: <pkg>`

You tried `tpkg remove zlib` but `python3` depends on it. Options:

1. Remove the dependent first: `sudo tpkg remove python3`, then `sudo tpkg remove zlib`.
2. Force it: `sudo tpkg remove zlib --force` (python3 will break).

### `command conflict: ls`

You installed `coreutils` (which has `ls`), then installed another package with its own `ls`. tpkg warns:

```
warning: 'ls' is provided by coreutils; coreutils-new's ls will be used
```

The second package wins. `tpkg which ls` shows the current owner. Use `--force` to install silently.

### `disk full`

`df -h` shows `Use%` at 100%. Remove unused packages:

```bash
sudo tpkg remove <large-unused-package>
```

Or increase the VM's RAM (`-m 4096`).

### `build too old` / `min_build`

Same as "min_build" above. Update tOS first.

### `not found` (remove)

The package is not installed. Check `tpkg list`.

### `download failed`

Usually a transient network error. Retry. If persistent, check the server status on GitHub.

### `could not save` / `could not open`

The package's files could not be written. Usually `disk full` or a read-only filesystem (live environment without a disk).

## Debugging tips

- Run `tpkg` with `TPKG_TRACE=1` for verbose output (not implemented yet, but planned).
- Keep the serial log: `qemu ... -serial file:serial.log`.
- Check `/tmp/tpkg-*.log` for partial downloads.

## Reporting a bug

If you hit an error not in this table:

1. Run the failing command with `TPKG_TRACE=1` (when available).
2. Note the build number (`version` in the Terminal).
3. Open an issue on GitHub with the exact command, the full output, and the serial log if possible.
