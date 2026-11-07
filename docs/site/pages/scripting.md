title: MicroPython and T#
lede: tOS has two scripting languages built into the kernel: MicroPython, a small Python, and T#, a minimal language of its own. Both can run shell commands, read and write files, open apps and play audio.
---
## MicroPython

MicroPython 1.24 is part of the kernel, so it needs no package. Start the REPL with:

```
python
```

```
MicroPython REPL on tOS
>>> print("hello from tOS")
hello from tOS
>>> import math
>>> math.sqrt(42)
6.480741
>>> import os
>>> os.listdir("/")
['programs', 'mnt']
>>> import time
>>> time.ticks_ms()
1843
```

Press **Ctrl+D** to leave. `python <file.py>` runs a script. For real programs, use CPython (`python3`, from the `python3` package).

### Modules

| Module | What it has |
|---|---|
| `math`, `random`, `struct`, `array`, `collections` | The usual basics (`namedtuple` in collections) |
| `ubinascii`, `json`, `ure` | Hex and base64, JSON (`loads`, `dumps`), regular expressions |
| `os` | `getcwd`, `chdir`, `listdir`, `mkdir`, `remove`, `rename`, `stat`, `path.exists`, `path.isdir`, `path.isfile`, `path.join`, `path.basename`, `path.dirname` |
| `time` | `ticks_ms`, `ticks_us`, `ticks_diff`, `time`, `sleep`, `sleep_ms`, `sleep_us` |
| `socket` | TCP and UDP sockets: `connect`, `bind`, `send`, `recv`, `sendto`, `recvfrom`, `getaddrinfo` |
| `tos` | The tOS scripting API (below) |
| `tosgui` | A minimal toolkit for your own windows (below) |

### Files

`open(path, mode)` works on any mounted file system. Modes: `r`, `w` (create and truncate), `a` (append), `r+`, `w+`.

```python
with open("/mnt/hello.txt", "w") as f:
    f.write("hello tOS\n")
with open("/mnt/hello.txt") as f:
    print(f.read(64))
```

Modules saved on any mounted file system can be imported: save `mylib.py` and `import mylib`.

### Limits

MicroPython has a 128 KB heap, single-precision floats and arbitrary-precision integers, and no threads. It writes its output to the screen and to the serial port.

## The scripting API

MicroPython (module `tos`) and T# (built-in functions) share one API for running commands and touching files, the desktop and the processes:

| What | MicroPython | T# | Notes |
|---|---|---|---|
| Run a shell command | `tos.exec(cmd)` | `calistir(cmd)` | Returns the output as a string |
| Read a file | `tos.read(path)` | `dosyaoku(path)` | |
| Write a file | `tos.write(path, data)` | `dosyayaz(path, data)` | |
| Make a folder | `tos.mkdir(path)` | `klasoryap(path)` | |
| Delete a file or folder | `tos.delete(path)` | `dosyasil(path)` | |
| Does a path exist | `tos.exists(path)` | `dosyavarmi(path)` | |
| List a folder | `tos.list(path)` | `listele(path)` | A list in MicroPython, text in T# |
| Open an app | `tos.open_app(name)` | `uygulamaac(name)` | `notepad`, `paint`, `files`, `viewer`, `calculator`, `clock`, `about`, `diskutil`, `taskmgr`, `mediaplayer`, `netmon`, `snake`, `terminal` |
| List processes | `tos.ps()` | `surecler()` | `pid name state` lines |
| End a process | `tos.kill(pid)` | `sureldur(pid)` | Not the idle task or yourself |
| Uptime in seconds | `tos.uptime()` | `calismasuresi()` | |
| Fetch a URL (HTTP) | `tos.http_get(url)` | `agetir(url)` | The body only; up to 8 KB in MicroPython |
| Play audio | `tos.play(path)` | `sescal(path)` | WAV, MP3, M4A/AAC. Waits until it ends |
| Stop audio | `tos.stop_audio()` | `sesdurdur()` | |
| Set volume | `tos.set_volume(vol)` | `sesseviyesi(vol)` | 0 to 100 |
| Is audio playing | `tos.audio_playing()` | `caliyormu()` | |

```python
import tos
print(tos.exec("ls /"))
tos.write("/tmp/note.txt", "made by a script")
print(tos.read("/tmp/note.txt"))
tos.open_app("notepad")
```

## Your own window: tosgui

`tosgui` opens a window and draws text and buttons in it:

```python
import tosgui as tg

tg.open("My App")
tg.text(2, 2, "Hello from MicroPython!")
tg.button(2, 4, "Quit", tg.WHITE, tg.RED)

while True:
    click = tg.poll_click()
    if click and click[1] == 4 and 2 <= click[0] <= 9:
        break
    tg.update()

tg.close()
```

| Function | What it does |
|---|---|
| `tg.open(title)`, `tg.close()` | Open and close the window |
| `tg.clear()` | Clear it |
| `tg.text(x, y, s, fg, bg)` | Draw text at a position |
| `tg.button(x, y, s, fg, bg)` | Draw a `[ s ]` button |
| `tg.poll_click()` | `(x, y)` of a click, or `None` |
| `tg.poll_key()` | An ASCII code, or `None` |
| `tg.input(x, y, prompt)` | Read a line |
| `tg.has_focus()` | Whether the window has focus |
| `tg.update()` | Give a frame to the rest of the system |
| `tg.BLACK` ... `tg.WHITE` | The 16 colour constants |

A demo ships at `/programs/tosgui_demo.py`: `python /programs/tosgui_demo.py`.

## T#

T# (T# 4.1 Lite) is a minimal language inside the kernel: arithmetic, conditions and loops, plus the functions in the table above (their names are Turkish). Start it with `tsharp`, or run a file with `tsharp <file>`.

```
tsharp
>>> sesseviyesi(70)
>>> sescal("/mnt/song.mp3")
```

Calls cannot be nested inside expressions: write `sescal(path)` on a line of its own.

## When to use what

| You want to... | Use |
|---|---|
| A quick script that touches the desktop or files, with nothing installed | MicroPython (`python`) |
| Real programs, libraries, pip | CPython (`python3`: [Python](/tos/dev-python)) |
| Scripts with pipes and tools | `sh` or `bash`: [Shell scripting](/tos/dev-shell) |
