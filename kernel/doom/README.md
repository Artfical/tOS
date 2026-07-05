# DOOM port — licensing

Everything in this directory except `port/` is vendored source from
[doomgeneric](https://github.com/ozkl/doomgeneric) (itself based on
Chocolate Doom / the original id Software DOOM source id released in
1997/1999). It is licensed under the **GNU General Public License
version 2** — see `LICENSE` in this directory for the full text.
`port/` (the tOS platform glue: `doomgeneric_tos.c`, `doom_main.c`,
`doom_compat.c` and the small compatibility headers) is original code
written for this project, also GPLv2-compatible.

## The WAD file (`assets/doom1.wad`)

The shareware version of DOOM's game data (`doom1.wad`, episode 1
only) has always been freely distributable under id Software's own
original terms — this is why "doom1.wad" is one of the most widely
mirrored files on the internet and why every doomgeneric-style port
bundles it directly rather than requiring the user to supply it. It is
**not** GPL-licensed and is **not** the full retail game — it's
id Software's own promotional shareware release, distinct from (and
much smaller than) the registered/retail IWAD.

If you own a legitimate copy of the registered DOOM, Doom II, or Final
DOOM data, you can replace `assets/doom1.wad` with that IWAD (rename it
to match, or update the `-iwad` path in `kernel/doom/port/doom_main.c`)
to play the full game instead of just the shareware episode.

## Known limitations (see the main README's "Linear framebuffer
graphics" section for more)

- No sound yet (`i_sound.c` compiles but has no audio backend wired
  up to tOS's `kernel/audio/`) — DOOM runs silently.
- No save/load persistence (`M_MakeDirectory()`'s save directory is a
  no-op) — games can't be saved between runs yet.
- Returning to the desktop after playing doesn't work reliably; expect
  to `reboot` afterward.
