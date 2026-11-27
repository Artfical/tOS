# Pin windows, Trash restore, quick settings

## files | New
- Put Back: in the Trash, select one or more items and click **Put Back** to return them to the folders they came from. tOS keeps the original place of each trashed item in `.Trash/.restore`, so it works after a restart too.

## desktop | New
- Pin a window to keep it above the others: right-click its title bar and choose **Pin (always on top)**, or use the **Window** menu. A pinned window stays on top until you unpin it. The title-bar right-click menu also has Minimize and Close.

# Search, Alt+Tab, window snapping, more Spaces

## desktop | New
- Search: Ctrl+Space (or the top right corner of the screen). Type, and apps, settings pages and files from your home, Desktop, Documents and Downloads folders are listed; Up and Down choose, Enter opens, Esc closes.
- Window switcher: Alt+Tab shows a picture of every window; each Tab goes to the next, and letting go of Alt brings that window forward (on its own Space too).
- Snapping: drag a window to the left or right edge of the screen for a half, to the top for the whole screen; a preview shows where it will land and the window glides there.
- Hot corners: rest the pointer in the top left corner for Mission Control, in the top right corner for Search.
- Up to 12 windows (6 before).
- Each Space can have a wallpaper of its own: pick one in Settings, Desktop & Dock while you are in that Space.

## settings | New
- "Dark from 19:00 to 07:00" in Appearance switches between light and dark by itself.

# Spaces, Mission Control, your own themes

## desktop | New
- Mission Control: Ctrl+Up or F3. The windows of a Space fly out to a grid (smoothly scaled, titles under them) and the Spaces show along the top. Click a window to bring it forward, click a Space to go there, drag a window onto a Space to move it (onto + for a new one), click + to add a Space (four at most), the minus in a Space's corner closes it. The Spaces along the top show the real windows, not placeholders. Esc, Ctrl+Down or F3 leave.
- Spaces: Ctrl+Left and Ctrl+Right slide the desktop sideways like the Mac's while the Dock and the top bar stay. A new window opens in the current Space, and a click on a Dock icon whose app is in another Space goes there. (In a Terminal running a Linux program, use Ctrl+Alt+arrows.)

## settings | New
- Your own themes: a folder `~/.themes/NAME` with a `theme.conf` (`dark=`, `accent=RRGGBB`, `wallpaper=`, `icons=`) and, if you like, an icons folder of PNGs named like the pictures they replace (`icon48_files.png`). They show as buttons in Settings > Appearance under the four ready-made themes, and the choice is kept. Examples: `docs/themes` (Nord, Sunset).

## terminal | Developers
- The animations are recorded on video by GitHub Actions on every change of the desktop (the release "animations"): `tools/record-animations.py` drives QEMU (`tools/qemu_drive.py`) from a kernel built with `make ANIM_SLOW=12`, which runs them twelve times slower so the screen can be followed, and plays them back at real speed.
- `gfx_overlay_add()` puts pictures in front of the icon packs; `window_t` has a `space`; Ctrl+arrows and F3 reach the desktop before any app (`keyboard_hotkey`).

## terminal | Documentation
- New page: Spaces and Mission Control. Settings explains your own themes and the theme.conf format.
