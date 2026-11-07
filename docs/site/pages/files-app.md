title: Files
lede: Files is tOS's file manager. You use it to browse folders, open files, copy and move them, and to find disks.
icon: files
---
![Files](shot:finder.png)

## The window

A Files window has:

- A **sidebar** on the left with *Favourites* (Home, Desktop, Documents, Downloads, Pictures) and *Locations* (the tOS disk, Programs, Temporary and the Trash). Disks you plug in show up under Locations.
- A **toolbar** with back and forward buttons, the name of the current folder, buttons for the view, and a **Search** box.
- The **files** in the middle, as icons or as a list.
- A **path bar** at the bottom that shows where you are, and how many items there are or how many are selected.

## Getting around

| To do this | Do this |
|---|---|
| Open a folder or a file | Double-click it |
| Go back and forward | The arrow buttons at the top left |
| Go straight to a place | Click it in the sidebar |
| Go up one level | Click the folder above in the path bar |
| Find a file | Type in the Search box |

Files opens each kind of file in the app that fits it: text files in Notepad and pictures in the Image Viewer, for example.

## Selecting

Click a file to select it. **Ctrl+click** adds a file to the selection or removes it. **Shift+click** selects a range, and so do Shift with the arrow keys. The path bar tells you how many are selected.

![Two files selected: drag them together](shot:drag_multiselect.png)

## Copying, moving, renaming, deleting

- **Copy**, **cut** and **paste** work on whole selections.
- **Rename** changes a file's name.
- **New Folder** makes a folder in the current one.
- **Delete** moves files to the **Trash**. Open the Trash from the sidebar or the dock, and use **Empty Trash** to delete them for good.
- **Drag and drop** moves a file to another folder in the window, or out of the window to the desktop, to an app, or to the Trash. See [The desktop](/tos/desktop).

![The Trash, opened in Files](shot:drag_trash_contents.png)

## Disks

Files works on every file system tOS understands: the tOS disk, FAT16/32, exFAT, ext2/3/4, NTFS, Btrfs and XFS. A disk you plug in (a USB stick, for example) is mounted by itself at `/Volumes/<label>`, appears on the desktop and in the sidebar, and has an **eject** button. Eject it before you pull it out.

![A USB disk in the Files sidebar, with its eject button](shot:usb_finder.png)

See [Notifications and USB disks](/tos/notifications) and [Files, disks and users](/tos/filesystems).

## Light and dark

Files, like the rest of the desktop, follows the appearance in Settings. It looks like this in the dark appearance:

![Files in the dark appearance](shot:icons/files.png)

## Keyboard and mouse

| To do this | Do this |
|---|---|
| Select one file | Click it |
| Add or remove a file from the selection | Ctrl+click |
| Select a range | Shift+click, or Shift with the arrow keys |
| Move between files | The arrow keys |
| Open | Double-click |
| Drag out of the window | Press on a file, move the pointer out, release over the target |

## Tips

- A file you save from an app lands where the file dialog was pointing. The dialog is a small Files window, with the same sidebar.
- Everything in Files can also be done in the Terminal with `ls`, `cp`, `mv`, `rm` and `mkdir`; see [Terminal and shell](/tos/terminal).
- Packages live in `/system/pkg`.
