# Spaces, Mission Control, your own themes

## desktop | New
- Mission Control: Ctrl+Up or F3. The windows of a Space fly out to a grid (smoothly scaled, titles under them) and the Spaces show along the top, with pictures of the real windows. Click a window to bring it forward, click a Space to go there, drag a window onto a Space to move it (onto + for a new one), click + to add a Space (four at most), the minus in a Space's corner closes it. Esc, Ctrl+Down or F3 leave.
- Spaces: Ctrl+Left and Ctrl+Right slide the desktop sideways like the Mac's while the Dock and the top bar stay. A new window opens in the current Space, and a click on a Dock icon whose app is in another Space goes there. (In a Terminal running a Linux program, use Ctrl+Alt+arrows.) Each Space can have a wallpaper of its own.
- Search: Ctrl+Space (or the top right corner). Type to find apps, settings pages and files from your home, Desktop, Documents and Downloads folders; Up and Down choose, Enter opens, Esc closes.
- Window switcher: Alt+Tab shows a picture of every window; each Tab goes to the next, and letting go of Alt brings that window forward (on its own Space too).
- Snapping: drag a window to the left or right edge for half of the screen, to the top for all of it; a preview shows where it will land and the window glides there.
- Pin a window above the others: right-click its title bar (or use the Window menu) and choose Pin; the same menu has Minimize and Close.
- Hot corners: the top left corner shows Mission Control, the top right corner Search.
- The speaker in the top bar mutes and unmutes the interface sounds.
- Quick settings in the Notification Centre (click the clock): Dark/Light and Sound on/Muted, beside Do Not Disturb.
- Up to 12 windows at once (6 before).

## settings | New
- Your own themes: a folder `~/.themes/NAME` with a `theme.conf` (`dark=`, `accent=RRGGBB`, `wallpaper=`, `icons=`) and, if you like, an icons folder of PNGs named like the pictures they replace (`icon48_files.png`). They show as buttons in Settings > Appearance under the four ready-made themes, and the choice is kept. Examples: `docs/themes` (Nord, Sunset).
- "Dark from 19:00 to 07:00" in Appearance switches between light and dark by itself.

## terminal | Fixed
- Wine: running a Windows program as a normal user (not root) works. The first run, and every run after, fixes the permissions of the folders in `drive_c`, so the old "Permission denied ... windows/system32" error is gone, and a `.wine` folder made before the fix is repaired. `wine` 10.0 package updated on update.artfical.com.

## files | New
- Put Back: in the Trash, select one or more items and click Put Back to return them to the folders they came from. tOS keeps the original place of each trashed item in `.Trash/.restore`, so it works after a restart too.

## terminal | Developers
- The animations are recorded on video by GitHub Actions on every change of the desktop (the release "animations"): `tools/record-animations.py` drives QEMU (`tools/qemu_drive.py`) from a kernel built with `make ANIM_SLOW=12`, which runs them twelve times slower so the screen can be followed, and plays them back at real speed.
- `gfx_overlay_add()` puts pictures in front of the icon packs; `window_t` has a `space` and a `pinned`; Ctrl+arrows, F3 and Ctrl+Space reach the desktop before any app (`keyboard_hotkey`).

## terminal | Documentation
- New page: Spaces and Mission Control (search, Alt+Tab, snapping, hot corners). Settings explains your own themes and the theme.conf format; Files explains Put Back.
- Fresh screenshots of the desktop, Spaces, search and Alt+Tab are taken by GitHub Actions (the "screenshots" release) and kept in `docs/screenshots`.