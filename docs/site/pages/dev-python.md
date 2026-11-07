title: Python
lede: tOS has CPython from 3.9 to 3.14, each with pip and a nearly complete standard library, plus MicroPython built into the kernel. This page shows how to use them.
---
## Install Python

```
sudo tpkg install python3
python3 --version
```

`python3` is **Python 3.14**. It brings the commands `python3`, `python`, `pip`, `pip3` and `pydoc3`. The older versions are separate packages: `python3.13`, `python3.12`, `python3.11`, `python3.10` and `python3.9`; each brings its own versioned commands (for example `python3.13` and `pip3.13`) so that several can be installed together.

```
sudo tpkg install python3.9
python3.9 -c "import sys; print(sys.version)"
```

## The standard library

The Python packages are built with the parts programs expect: `ssl` (OpenSSL 3, with the certificates from the `ca-certificates` package), `sqlite3`, `zlib`, `bz2`, `lzma`, `ctypes`, `readline`, `curses`, `json`, `hashlib`, `socket`, `threading`, `subprocess` and the rest. A quick check:

```
python3 -c "import ssl, sqlite3, zlib, bz2, lzma, ctypes, json, hashlib; print('ok', ssl.OPENSSL_VERSION, sqlite3.sqlite_version)"
```

## Scripts

Save a file `hello.py` that contains `print("hello from Python")` (with Notepad, vim or `edit`), then run it:

```
python3 hello.py
```

A script that starts with `#!/usr/bin/env python3` runs directly after `chmod +x`.

## pip

`pip` is included. Packages with C extensions need the compiler (`gcc`, and the `-dev` packages of the libraries they use).

```
python3 -m pip --version
python3 -m pip install <package>
```

## Windows with tkinter

`tkinter` (Tk 8.6) is in a separate package for each version: `python3-tk` for 3.14, and `python3.13-tk`, `python3.12-tk` and so on. They bring the toolkit and need the X server:

```
sudo tpkg install xorg tcltk python3-tk
tos-x python3 -c "import tkinter; tkinter.Tk().mainloop()"
```

See [Windowed programs](/tos/dev-x11).

## A web server in a few lines

```
python3 -m http.server 8000
```

serves the current folder. Python's `http.server`, `socketserver`, `ssl`, `urllib` and `asyncio` all work on tOS's sockets, including TLS servers.

## Threads and processes

Threads (`threading`), subprocesses, pipes and signals work, because the compatibility layer provides them: see [Linux programs](/tos/linux-programs).

## The built-in MicroPython

The kernel has its own, much smaller Python: `python` in the shell starts **MicroPython** (without installing anything), with modules that script the desktop. See [MicroPython and T#](/tos/scripting). The `python3` package also brings a command named `python`, the same name; use `python3` to be sure you get CPython.

## More examples

A script that reads a web page over HTTPS:

```
import urllib.request
with urllib.request.urlopen("https://example.com") as r:
    print(r.status, len(r.read()))
```

A small SQLite database:

```
import sqlite3
db = sqlite3.connect("/tmp/demo.db")
db.execute("create table if not exists t (n integer)")
db.execute("insert into t values (42)")
print(db.execute("select n from t").fetchall())
```

Hashing and compression:

```
import hashlib, zlib
print(hashlib.sha256(b"tOS").hexdigest()[:16])
print(len(zlib.compress(b"abc" * 100)))
```

## Related

- [Packages](/tos/packages) and [Developing on tOS](/tos/developing).
