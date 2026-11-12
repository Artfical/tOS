title: Qt
lede: Qt 6.8 runs on tOS: Core, Gui, Widgets, Network, Xml and Sql, with the headers, qmake, moc and CMake support to build Qt programs on tOS with gcc. This page shows how to install it, build a program and run it.
---
## Install

```
sudo tpkg install qt6-base gcc
```

`qt6-base` brings what it needs: the `xorg` server and libraries, `xcb-libs`, `xcb-util`, `xkbcommon`, `fontconfig` and `fonts-dejavu` (Qt draws its own text with these fonts). `gcc` (15.3.0-3 or later) knows where Qt's headers and libraries are.

## Build a program

A program with a signal and a slot, `main.cpp`:

```
#include <QApplication>
#include <QPushButton>
#include <QObject>
#include <cstdio>
class Counter : public QObject {
    Q_OBJECT
public:
    int n = 0;
public slots:
    void inc() { n++; }
};
int main(int argc, char **argv) {
    QApplication a(argc, argv);
    Counter c;
    QPushButton b("Click me");
    QObject::connect(&b, &QPushButton::clicked, &c, &Counter::inc);
    b.click(); b.click();
    printf("n=%d\n", c.n);
    return 0;
}
#include "main.moc"
```

With CMake (`cmake` and `ninja` packages):

```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/system/pkg/qt6-base
cmake --build build
```

with a `CMakeLists.txt` of

```
cmake_minimum_required(VERSION 3.21)
project(demo CXX)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
add_executable(demo main.cpp)
target_link_libraries(demo Qt6::Widgets)
```

With qmake: `qmake6 && make`. With g++ alone, for a program that needs no `moc`:

```
g++ prog.cpp -fPIC -I/system/pkg/qt6-base/include -I/system/pkg/qt6-base/include/QtCore -lQt6Core -o prog
```

(add `QtGui` and `QtWidgets` include folders and `-lQt6Widgets -lQt6Gui` for windows). `moc`, `uic` and `rcc` are on the PATH after installing `qt6-base`.

Compiling Qt programs needs memory and time on tOS: Qt's headers are big. Give tOS 2 GB or more.

## Run a window

Qt picks a platform plugin with `QT_QPA_PLATFORM`:

| Platform | What it does |
|---|---|
| `xcb` | Windows on the X server: `tos-x ./prog` (the default when `DISPLAY` is set) |
| `linuxfb` | Draws straight into the tOS window (`/dev/fb0`), with no X server |
| `offscreen` | No window: draws into memory (for tests and `QWidget::grab()`) |

```
tos-x ./demo
QT_QPA_PLATFORM=linuxfb ./demo
```

## What this build has

| Part | Support |
|---|---|
| Modules | Core, Gui, Widgets, Network, Xml, Sql (no SQLite driver), Concurrent, PrintSupport |
| Painting | The software raster engine (no OpenGL, no GPU) |
| Text | FreeType and fontconfig, with the DejaVu fonts |
| Not here | OpenGL, D-Bus, OpenSSL, Qt Quick, QtMultimedia, Qt WebEngine, printing to real printers |

## Status

On tOS itself: a Qt Core program and a Widgets program (with the `offscreen` platform, drawing text and buttons) run, and `g++` on tOS compiles and links a Qt program that then runs. The `xcb` platform (windows on `tos-x`) was tried on a Linux host with a virtual X server and works there; the `xcb` and `linuxfb` windows have not been checked in a tOS window yet.

## Source and licence

Qt is under the LGPL-3.0 (or the GPL-2.0 or GPL-3.0). It is linked dynamically, so you can replace it with your own build. The package carries the licence texts (and those of the parts Qt bundles: harfbuzz, libpng, libjpeg, PCRE2, zlib and others), a `NOTICE.txt` that says how it was built, and the Qt 6.8.2 source tarball. Its repository: [github.com/Artfical/tpkg-qt6-base](https://github.com/Artfical/tpkg-qt6-base). The libraries it needs have repositories too: `tpkg-xcb-libs`, `tpkg-xcb-util`, `tpkg-xkbcommon`, `tpkg-fontconfig`, `tpkg-fonts-dejavu`.

## Related

- [Windowed programs (X11, Tk)](/tos/dev-x11), [SDL 2](/tos/dev-sdl) and [C and C++](/tos/dev-c).
