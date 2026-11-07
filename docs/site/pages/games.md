title: Games and demos
lede: tOS comes with DOOM and Wolfenstein 3D (their shareware episodes), Snake, 2048, a rotating 3D cube, and a few graphics tests.
icon: doom, snake, 2048
---
## ![](icon:doom) DOOM

![DOOM on tOS](shot:doom.png)

tOS runs the **shareware episode of DOOM**. It is a port of the DOOM engine (doomgeneric) with tOS's own glue for the screen, keyboard and timing; the game data is the freely distributable shareware file. Start it from the Launcher, or from the Terminal:

```
doom
```

| To do this | Press |
|---|---|
| Move and turn | The arrow keys |
| Fire, open doors | The usual DOOM keys (Ctrl to fire, Space to open) |
| Return to the desktop | Ctrl+C |

DOOM draws straight to the screen, in real pixels: while it runs, the desktop's own display mode is replaced by DOOM's. Close the window or press Ctrl+C and the desktop comes back.

## Wolfenstein 3D

tOS runs the **shareware episode of Wolfenstein 3D** too, through a port of Wolf4SDL with a small SDL compatibility layer made for tOS. Start it with:

```
wolf3d
```

Turning works with the keyboard. (The mouse is not used for turning.)

### About the ports

The DOOM engine is **doomgeneric** (GPL-2.0), with tOS's own code for drawing a frame, reading the keyboard and keeping time. The game data is the freely distributable shareware `doom1.wad`, which is part of the disc image. Wolfenstein 3D is **Wolf4SDL** (GPL-2.0) with id Software's shareware episode; because the engine calls SDL everywhere, tOS carries a small SDL-compatible layer of its own. Both are shown in the licence list (`tfire:licenses`) with their licences.

## ![](icon:snake) Snake

A classic: steer the snake with the arrow keys, eat to grow and score, avoid walls and your own tail. The game speeds up as the score climbs. **P** pauses and **Enter** starts again after a game over. The best score of the session is kept.

## ![](icon:2048) 2048

![2048](shot:2048.png)

Slide the tiles with the arrow keys (or W, A, S, D). Tiles with the same number merge when they meet. Reach 2048. **Enter** restarts at any time. The best score of the session is kept.

## The 3D cube

```
3d
```

shows a rotating, filled and shaded cube drawn by a software 3D renderer: perspective projection, back-face culling, a per-pixel depth buffer and lighting. Press **Escape** or **Ctrl+C** to leave.

## Graphics test

```
vgatest
```

switches to a real pixel graphics mode, draws a gradient with a crosshair, waits for a key, and returns to the desktop. It checks that the display works the way tOS needs.

## The crash screen

```
crash
```

shows the full-screen crash display with a made-up error. Nothing is wrong: it is a demo. Press any key to return. This is what you would see if the kernel really failed.

![The crash screen](shot:crash.png)

## Related

- Sound for the games: [Audio and the media player](/tos/audio).
- The Linux software on tOS is not limited to games: see [Linux programs](/tos/linux-programs).
