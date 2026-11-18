# tFire: tabs, find, bookmarks and history

## tfire | New
- Tabs: Ctrl+T opens one, Ctrl+W closes it, Ctrl+Shift+T opens the last closed one again, Ctrl+Tab and Ctrl+Shift+Tab switch, Ctrl+1 to Ctrl+9 jump. A strip of tabs shows from the second tab on (up to eight), and the tabs that were open come back when tFire is opened again.
- Find in a page: Ctrl+F. Every match is lit, the current one in orange, with a count; Enter and Shift+Enter move between them.
- Bookmarks: Ctrl+D or the star in the address bar; `tfire:bookmarks` (Ctrl+Shift+O) lists and removes them, and the start page shows them.
- History: the pages you visit are kept (300 at most) in `tfire:history` (Ctrl+H), where entries can be removed or all cleared; the start page shows the latest.
- The address bar suggests bookmarks and visited pages as you type (Up and Down choose, Enter goes), and has a caret you can move: Left and Right, a click, Ctrl+A, Ctrl+C, Ctrl+X, Ctrl+V.
- Bookmarks, history and the open tabs are plain text files in `.tfire` in your home folder.

## tfire | Changed
- The keyboard driver remembers which of Shift, Ctrl and Alt were held when each key was pressed (`keyboard_last_mods`), so an app can tell Ctrl+T (the character 0x14) from the right arrow even if Ctrl is let go first. tFire uses it for its shortcuts.

## terminal | Documentation
- tFire's page in the documentation covers tabs, find, bookmarks, history and the address bar; the docs also have pages for SDL 2, Qt and PyQt6, DRM and KMS, and every package has its own repository on GitHub (github.com/Artfical/tpkg-NAME).

## terminal | Programs (tpkg install ...)
- New packages: qt6-base 6.8.2 (with xcb-libs, xcb-util, xkbcommon, fontconfig, fonts-dejavu), pyqt6 6.8.1, sdl2 2.30.9; gcc 15.3.0-3 finds their headers and libraries on its own (`sudo tpkg install qt6-base gcc`).
