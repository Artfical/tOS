# Themes and icon packs

## settings | New
- Themes: System Settings > Appearance has a Theme row with four ready-made themes. One click sets light or dark, the accent colour, the wallpaper and the icons together: **tOS** (dark, blue, Dusk), **Classic** (light, Graphite, the old icons), **Midnight** (dark, purple, Graphite) and **Ocean** (light, green, Ocean). The one in force is outlined; changing any single setting afterwards leaves the row without a selection.
- Icon packs: a new Icons switch chooses **WhiteSur** (the macOS-like icons, as now) or **Classic** (tOS's own icons from before build 577). It changes every app icon, the Dock, the launcher, Files, notifications and the document, folder and disk icons at once.
- The choice is kept in `~/.desktop` (`icons=`), like the other desktop settings.

## settings | Changed
- The Appearance page no longer has the two Light and Dark preview pictures (the Light/Dark switch and the themes replace them).

## terminal | Developers
- `gfx_set_icon_pack()` and `gfx_icon_pack()` in `gfx.h`: `gfx_image()` looks in the selected pack first, so any app gets the pack's icons without changes. `ui_presets` in `uiapp.h` lists the themes; `uihost_set("theme", n)` applies one. `tools/gen_classic_icons.py` builds the Classic pack (`ui_classic.c`, 59 icons, 78 KB packed) from the old and the current `ui_assets.c`.

## files | New
- Double-clicking a Windows program (.exe) or installer (.msi) opens it with Wine, once Wine is installed.

## terminal | Changed
- Kernel support for Wine 10: faults reach signal handlers, per-thread GS base, `tgkill`, socket `stat`, shared memory that outlives its mapper, `epoll_create`, `getxattr`, file descriptors passed with SCM_RIGHTS, up to 40 processes and 512 open files.
