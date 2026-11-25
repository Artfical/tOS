title: Spaces and Mission Control
lede: Spread your windows over several desktops, called Spaces, and see all of them at once with Mission Control, with the animations of the Mac.
---

## Mission Control

Press **Ctrl+Up** or **F3**. Every window of this Space flies out to a grid and the Spaces appear along the top. Press **Esc**, **Ctrl+Down**, **F3** again, or click an empty place to go back.

- **Click a window** to bring it forward: the windows fly back and that one is on top.
- **Click a Space** to go there.
- **Drag a window onto a Space** to move it there. Dragging it onto **+** makes a new Space for it.
- **Click +** to add a Space (up to four).
- Move the pointer over a Space and click the **minus** in its corner to close it; its windows go to the Space before it.

The windows are drawn small with a smooth filter and keep their titles under them. Everything moves with an ease-out, like the Mac's.

## Spaces

A new window opens in the Space you are in. Switch with **Ctrl+Left** and **Ctrl+Right**: the desktop slides sideways while the Dock and the top bar stay. A click on a Dock icon whose app is open in another Space takes you there.

In a Terminal that runs a Linux program (vim, htop), Ctrl+arrows belong to the program; add Alt (**Ctrl+Alt+arrows**) to switch Spaces from there.

| To | Press |
|---|---|
| Show Mission Control | Ctrl+Up or F3 |
| Leave it | Esc, Ctrl+Down or F3 |
| The Space on the left or right | Ctrl+Left, Ctrl+Right |

## The animations on video

Every change of the desktop is recorded by GitHub Actions (the genie, Mission Control, Spaces, the themes and more) in [tOS-animations.mp4](https://github.com/Artfical/tOS/releases/download/animations/tOS-animations.mp4). The recording runs the desktop twelve times slower and plays it back at its real speed: `tools/record-animations.py`, built with `make ANIM_SLOW=12`.

## Next

[The desktop](/tos/desktop) explains the Dock and the windows, and [System Settings](/tos/settings) the themes.
