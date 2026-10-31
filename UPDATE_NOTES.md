# Libraries for the compiler, and file locks

## terminal | Libraries
- gcc now finds the libraries below by itself: sudo tpkg install ncurses (or readline, sqlite, ...) and gcc prog.c -lncurses just works. Each is its own package with headers, static and shared libraries, and its source: zlib-dev, openssl-dev, liblzma-dev, ncurses, readline, libffi, bzip2, sqlite, expat, pcre2 and libyaml (they bring gcc along).
- Every one is tested inside tOS by compiling and running a program against it: 5 MB compressed and unpacked again, an RSA-2048 signature, a 20,000-row SQLite database with joins and full-text search, a terminal screen drawn with curses, regular expressions with JIT, YAML and XML documents, a function called through libffi.
- Also new: sqlite3, bzip2, pcre2grep, tput, infocmp and xmlwf commands.

## terminal | Files
- Programs can lock files: fcntl byte-range locks (F_SETLK, F_SETLKW, F_GETLK) and flock work between programs, and a program's locks go away when it ends. Databases need this (SQLite said "disk I/O error" without it).
