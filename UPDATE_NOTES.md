# Files that move, and tOS Recovery when you need it

## files | Files, Desktop and Trash
- Drag files and folders from Files to the Desktop, another folder, or another Files window.
- Select several items with Ctrl-click or a Shift-click range and move them together.
- Hold Alt/Option while dropping to copy instead of move; name conflicts get a new name.
- Deleting from Files moves items to the Trash. The Dock Trash shows when it is full; emptying it requires confirmation.
![Files moving to the Desktop, Trash and app icons](docs/videos/drag-drop.mp4)
![A file on the Desktop after dragging it from Files](docs/screenshots/drag_desktop.png)
![Two selected files moved together onto the Desktop](docs/screenshots/drag_multiselect.png)
![The full Trash icon in the Dock](docs/screenshots/drag_trash.png)
![The Trash opened in Files](docs/screenshots/drag_trash_contents.png)

## launcher | Open files from the Dock
- Drop text documents on Notepad, pictures on Image Viewer, and HTML pages on tFire.
- A Dock target highlights while a compatible file is dragged over it.
![A text file opened by dropping it on Notepad](docs/screenshots/drag_notepad.png)
![An image opened by dropping it on Image Viewer](docs/screenshots/drag_viewer.png)
![A local HTML file opened by dropping it on tFire](docs/screenshots/drag_tfire.png)

## recovery | tOS Recovery
- Run `bootrec` to restart straight into Recovery, or `bootrec -n` to choose Recovery at the next start.
- Hold Shift while tOS starts to enter Recovery instead of the login screen.
- The one-time `bootrec -n` request is consumed on the next start; Recovery still offers Install tOS, Disk Utility, and Try tOS in Live Environment.
![Bootrec and Recovery at startup](docs/videos/recovery.mp4)
![The bootrec command](docs/screenshots/recovery_bootrec_command.png)
![tOS Recovery selected at startup](docs/screenshots/recovery_shift.png)
