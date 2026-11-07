title: Recovery and the installer
lede: tOS Recovery is the first screen on a machine where tOS is not installed. From it you install tOS, repair or format disks, or try tOS without changing anything.
icon: rec_diskutil
---
## The boot screen

With a graphical display, tOS asks nothing in text. It shows its boot screen (the logo on white with a progress bar) and then one of two screens:

- **On a computer where tOS is not installed**, for example the ISO in a fresh virtual machine: **tOS Recovery**.
- **On an installed system**: the **login window**.

![The login window of an installed system](shot:login.png)

![Logging in: the account pictures and the password field](video:login.mp4)

Without a graphical display, the old text questions remain: keyboard layout, the text installer, GUI or command line, login.

## tOS Recovery

Recovery has three choices:

![tOS Recovery](shot:recovery.png)

| Choice | What it does |
|---|---|
| **Install tOS** | Installs tOS on a disk of the computer. The disk is erased |
| **Disk Utility** | Repairs a disk with First Aid, or erases it |
| **Try tOS in Live Environment** | Starts a live desktop straight from the image, without changing anything on the computer |

Recovery has no dock and opens no other apps. It cannot be opened from a running system, but you can ask for it at the next start (see below).

![A short video of Recovery](video:recovery.mp4)

## Installing tOS

Choose **Install tOS** and press Continue. The installer asks, one screen at a time:

1. **The keyboard layout.** With a picture of the keyboard and a field to try the layout in, so you can check that the keys are where you expect.
2. **The network.** Check that the machine is connected.
3. **The account.** Your full name, an account name made from it (you can change it), a password, and a picture, chosen from 29.
4. **The disk.** The disk tOS is installed on. Everything on it is erased.

![Choosing the keyboard, with a picture of it](shot:recovery_keyboard.png)

![The account: full name, account name, password and a picture](shot:recovery_account.png)

![Choosing the disk to install on](shot:recovery_disk.png)

Then tOS installs, shows that it is installed, and asks you to restart.

![Installed: restart to start tOS from the disk](shot:recovery_installed.png)

The first user you create joins the `sudo` group, so that you can install packages and change the system. The system files are copied to the disk and tOS starts from it from then on.

## Trying tOS without installing

Choose **Try tOS in Live Environment**. It asks for the keyboard layout and starts a live desktop. The user is `tos`, and **there is no password**, so `sudo` works without one. Everything works as in an installed system (you can install packages and run Linux programs) but nothing is written to a disk. When you restart, you are back at the beginning.

## The login window

On an installed system you see the login window: the account pictures, a password field, and **Restart** and **Shut Down**. The keyboard layout you chose at installation is used.

## Opening Recovery on an installed system

You can reach Recovery again on a machine where tOS is installed, to repair the disk or to reinstall:

- Run `bootrec` in the Terminal. It restarts into tOS Recovery.
- Run `bootrec -n` to open Recovery **only at the next start**, without restarting now.
- Hold **Shift** while the machine starts.

![bootrec -n: Recovery will open at the next start](shot:recovery_bootrec_command.png)

![Recovery, opened on an installed system](shot:recovery_shift.png)

## Disk Utility from Recovery

**Disk Utility** is the same app as on the desktop: it lists the disks, runs **First Aid** on a volume, and erases or formats a disk. See [Activity, Network and Disk Utility](/tos/system-apps).

## Next

- [Quickstart](/tos/quickstart): your first ten minutes.
- [The desktop](/tos/desktop): how to find your way around.
