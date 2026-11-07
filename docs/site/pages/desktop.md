title: The desktop
lede: tOS has a windowed desktop with a dock, a top bar and a set of built-in apps.
---
## The look

![The desktop: top bar and dock](shot:desktop.png)

The desktop has a frosted top bar with the clock, a dock with the apps, and windows you can drag by the title bar, resize from the corner, focus and minimise. It starts at 1280x720. **Settings** changes the size (1024x768 to 1920x1080) and the appearance, light or dark.

Right-click on the desktop for a menu: New Folder, New File, Open Files, Open Terminal, About This Computer, Change Background.

## The apps

![The launcher](shot:launcher.png)

![Files, in the light appearance](shot:finder.png)

![Settings, in the dark appearance](shot:settings.png)

![Paint](shot:paint.png)

![Activity, the task manager](shot:taskmanager.png)

![Network: addresses and live speed](shot:network.png)

![Disk Utility](shot:diskutil.png)

![tFire, the web browser](shot:tfire/home.png)

| App | What it does |
|---|---|
| Files | Browse, copy, move and open files, laid out like the Mac's Finder. Disks show in its sidebar |
| Terminal | The shell. See [Terminal and shell](/tos/terminal) |
| Settings | Appearance (light or dark), the screen size and more |
| tFire | The web browser: HTTPS and HTTP, pictures, back and forward, search |
| Notepad | Plain text editing |
| Paint | Drawing, with menus, layers and zoom, saving through the file dialog |
| Calculator, Clock | A calculator, and a clock with a stopwatch |
| Network | Your addresses and the live download and upload speed |
| Activity | The running processes, like a task manager |
| Disk Utility | Check, format and resize disks, with First Aid |
| Launcher | All the apps in one place |
| Games | 2048, DOOM (shareware) and Wolfenstein 3D |

## Notifications

![Notification Centre, opened from the clock](shot:notification_center.png)

![A notification that waits for an answer](shot:notification_buttons.png)

![Do Not Disturb: banners silenced, a moon in the top bar](shot:do_not_disturb.png)

Apps post notifications as banners at the top right. A banner leaves after a few seconds, unless it has buttons: then it waits for your answer. Everything stays in **Notification Centre**, which opens from the clock. **Do Not Disturb** silences the banners and the chime and shows a moon in the top bar.

From the Terminal or a script:

```
notify "Build finished" "tOS.iso is ready"
notify -b Install -b Later -w "Update ready" "A new build is ready to install."
notify dnd on
```

With `-w`, `notify` waits and prints which button was pressed.

## USB disks

![A USB disk on the desktop and in the Files sidebar, with an eject button](shot:usb_finder.png)

![Pulled out without ejecting](shot:disk_not_ejected.png)

A USB disk (FAT16/32, exFAT or ext2/3/4) that you plug in is mounted by itself at `/Volumes/<label>` and shows up on the desktop and in the Files sidebar, with an eject button. Eject before you pull it out; if you do not, tOS notices and tells you: "Disk Not Ejected Properly".

In QEMU, start with `-usb` and plug a disk in from the monitor:

```
(qemu) drive_add 0 if=none,id=ud,format=raw,file=usb.img
(qemu) device_add usb-storage,id=us,drive=ud
(qemu) device_del us
```

## Windows from Linux programs

Programs that draw their own window run inside a tOS window too, through the X server. See [Linux programs](/tos/linux-programs).
