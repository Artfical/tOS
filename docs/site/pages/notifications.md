title: Notifications and USB disks
lede: How apps tell you things, how to keep the quiet, and how tOS handles disks that you plug in and pull out.
icon: doc_disk
---
## Notifications

Apps post notifications as **banners** at the top right of the screen. A banner shows the app's icon, a title, up to two lines of text and up to two buttons.

![A banner with the app's icon and two buttons](shot:notifications.png)

- A banner without buttons slides away after about six seconds.
- A banner **with buttons** waits for your answer.
- Clicking a banner opens the app that sent it.
- Every notification stays in the **Notification Centre**, which opens from the clock in the top bar.

![A notification that waits for an answer](shot:notification_buttons.png)

![The Notification Centre, opened from the clock](shot:notification_center.png)

## Do Not Disturb

**Do Not Disturb**, a switch in the Notification Centre, silences the banners and the chime and shows a moon in the top bar. The Centre still collects everything that arrives, so you can read it later.

![Do Not Disturb: banners silenced, a moon in the top bar](shot:do_not_disturb.png)

## Posting a notification from the Terminal

The `notify` command posts one. This is how scripts tell you that something has finished:

```
notify "Build finished" "tOS.iso is ready"
notify -a paint "Picture saved" "Drawing.png was saved."
notify -b Install -b Later -w "Update ready" "A new build is ready to install."
notify dnd on
notify dnd off
```

| Option | What it does |
|---|---|
| `-a <app>` | Uses that app's icon and opens it when clicked |
| `-b <text>` | Adds a button (up to two). The banner then waits for an answer |
| `-w` | Waits, and prints which button was pressed (1 or 2), 3 for a click on the banner, or -1 when it was closed |
| `dnd on`, `dnd off` | Turns Do Not Disturb on and off |

## ![](icon:doc_disk) USB disks

A USB disk that you plug in (formatted FAT16 or FAT32, exFAT, or ext2/3/4) is **mounted by itself** at `/Volumes/<label>`. It appears on the desktop and in the sidebar of Files, with an **eject** button.

![A USB disk on the desktop and in the Files sidebar](shot:usb_finder.png)

### Ejecting

Click the eject button before you pull the disk out. Ejecting writes everything that is waiting to the disk and unmounts it. If you pull the disk out without ejecting, tOS notices and tells you: **Disk Not Ejected Properly**.

![Pulled out without ejecting: Disk Not Ejected Properly](shot:disk_not_ejected.png)

### In a virtual machine

In QEMU, start the machine with `-usb`. Then plug a disk image in from the QEMU monitor, and take it out again:

```
(qemu) drive_add 0 if=none,id=ud,format=raw,file=usb.img
(qemu) device_add usb-storage,id=us,drive=ud
(qemu) device_del us
```

## Tips

- Posting a notification from a long job is a good way to know when it ends: `make && notify "Build done" "make finished"`. (Write it with `&&` in a full shell, or run `notify` on its own after the job.)
- Do Not Disturb silences the banners and the chime only; apps keep posting to the Notification Centre.
- A banner with buttons keeps waiting, so answer it or close it.

## Tips

- Posting a notification from a long job is a good way to know when it ends: `make && notify "Build done" "make finished"`. (Write it with `&&` in a full shell, or run `notify` on its own after the job.)
- Do Not Disturb silences the banners and the chime only; apps keep posting to the Notification Centre.
- A banner with buttons keeps waiting, so answer it or close it.

## Other disks

Disks that are not removable (the ones you find with `disk list`) are handled with Disk Utility and the `disk` command: see [Activity, Network and Disk Utility](/tos/system-apps) and [Files, disks and users](/tos/filesystems).
