title: How commands reach your PATH
lede: The command table, name=target entries and the 'program argument' form, /usr/bin and /bin aliases for Linux programs, shebang interpreters, what happens when two packages bring the same name, tpkg which.
---
## The command table

When tpkg installs a package, it reads `tpkg.commands` and appends entries to `/system/path.tmbl`. This file is a simple `name=target` list, one per line.

Example from `coreutils`:

```
ls=bin/coreutils ls
cp=bin/coreutils cp
mv=bin/coreutils mv
sort=bin/coreutils sort
```

The `target` is a **relative path** under `/system/pkg/<name>/`. The shell expands it to `/system/pkg/coreutils/bin/coreutils ls` when you type `ls`.

## The 'program argument' form

Some packages ship a single binary that acts as a dispatcher. The command table entry includes the argument:

```
busybox=bin/busybox
```

When you run `ls` (which maps to `busybox ls`), the shell runs `/system/pkg/busybox/bin/busybox ls`. Busybox sees the first argument (`ls`) and dispatches to its `ls` applet.

This is how `coreutils` (single binary with many tools) and `busybox` work.

## The PATH

The shell builds its PATH from:

1. `/system/pkg/<name>/bin` for every installed package (in install order, last wins).
2. `/usr/bin` and `/bin` (symlinks to `/system/pkg/busybox/bin/busybox` for the built-in tools).
3. `/system/programs` (for the few built-in `.t` programs).

The shell caches the PATH. After `tpkg install`, run `hash -r` or open a new Terminal to see new commands.

## /usr/bin and /bin aliases

tOS provides `/usr/bin` and `/bin` as compatibility layers. They are populated by the `busybox` package:

```
/usr/bin/ls   -> /system/pkg/busybox/bin/busybox ls
/bin/sh       -> /system/pkg/busybox/bin/busybox sh
```

This means scripts that start with `#!/bin/sh`, `#!/usr/bin/env python3`, or `#!/bin/bash` work once the corresponding package is installed.

## When two packages bring the same name

If `coreutils` (installed first) provides `ls`, and then you `sudo tpkg install coreutils-new` (which also has `ls`), tpkg warns:

```
warning: 'ls' is provided by coreutils; coreutils-new's ls will be used
```

The last installed package wins. `tpkg which ls` shows the current owner:

```
tpkg which ls
# ls -> /system/pkg/coreutils-new/bin/coreutils ls
```

Use `--force` on install to suppress the warning:

```bash
sudo tpkg install coreutils-new --force
```

## The `which` command

`which <command>` is a shell builtin. It searches the current PATH and prints the first match:

```
which git
# /system/pkg/git/bin/git
```

`tpkg which <command>` does the same but uses the command table directly (ignoring shell cache):

```
tpkg which ls
# ls=/system/pkg/coreutils/bin/coreutils ls
```

## Shebang interpreters

Scripts starting with `#!/bin/bash` need the `bash` package. `#!/usr/bin/env python3` needs `python3`. The kernel's `execve` reads the shebang, finds the interpreter on PATH, and executes it. Because `/usr/bin` and `/bin` point to the package's binaries, standard shebangs work.

## Tips

- After `tpkg install`, run `hash -r` in the current shell to refresh its command cache.
- `tpkg which <cmd>` is the authoritative way to check what a command will run.
- If a command disappears after `tpkg remove`, run `hash -r` to clear the shell's stale cache.

## Related

- [What happens when you install](/tos/tpkg-install) — the install pipeline.
- [The package format](/tos/tpkg-format) — inside a `.tpkg`.
- [Dependencies](/tos/tpkg-dependencies) — how deps work.
