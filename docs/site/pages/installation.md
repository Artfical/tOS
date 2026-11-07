title: Installation
lede: tOS starts from a disc image. You can try it in a virtual machine in a minute, or write it to a USB stick and start a computer from it.
---
This page covers getting the image, starting it in QEMU, VirtualBox and VMware, writing a USB stick, what the machine needs, and how updates work. What happens after the image starts is on [Recovery and the installer](/tos/recovery).

## Get the image

Every commit on the main branch is built and published on the [Releases page](https://github.com/Artfical/tOS/releases) as **build @commit**, with `tOS.iso` attached. The newest one at the top is the one to take: there are no separate stable and beta channels, because every build is tested by starting it in a virtual machine before it is published.

You can also build the image yourself: see [Building tOS from source](/tos/build-tos).

## QEMU

QEMU is the quickest way to try tOS, and the one it is tested in the most:

```
qemu-system-x86_64 -cdrom tOS.iso -m 2048 -vga std -nic user,model=e1000
```

| Option | What it does |
|---|---|
| `-cdrom tOS.iso` | Uses the image as the disc to start from |
| `-m 2048` | Gives the machine 2 GB of memory. Programs and packages live in memory, so give it more if you plan to install a lot or to build software (`-m 4096`) |
| `-vga std` | The display the desktop needs (a VBE framebuffer) |
| `-nic user,model=e1000` | A network card with internet access through QEMU's built-in NAT |

More options you may want:

- **A disk to install on:** create one with `qemu-img create -f qcow2 tos.qcow2 8G` and add `-drive file=tos.qcow2,if=ide`. The installer lets you choose it.
- **Sound:** `-soundhw sb16` (or `make run-audio` from the source tree) gives a Sound Blaster 16.
- **A USB disk:** add `-usb`, then plug a disk in from the QEMU monitor (see [Notifications and USB disks](/tos/notifications)).
- **The kernel log:** `-serial file:serial.log` writes everything the kernel logs to a file. Useful for bug reports.
- **No window (for tests):** `-display none`.

## VirtualBox

1. Create a new machine, type **Other**, version **Other/Unknown (64-bit)**.
2. Give it at least 1 GB of memory (2 GB recommended).
3. Attach `tOS.iso` as the optical drive and start it.
4. For sound, choose the **ICH AC97** audio controller in Settings, Audio. tOS finds it by itself.

## VMware

Create a 64-bit machine of type "Other", give it at least 1 GB of memory, attach `tOS.iso` as the CD/DVD, and start it. The desktop uses VMware's VBE display at 1280x720.

![tOS running in VMware](shot:vmware_desktop.png)

## A USB stick

Write the image to a stick and start the computer from it. **This erases the stick.**

```
sudo dd if=tOS.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

Replace `/dev/sdX` with your stick. Check the name with `lsblk` first: writing to the wrong device destroys its data. On Windows, use a tool that writes disc images to USB sticks in raw mode. tOS is tested in virtual machines, so try those first before real hardware.

## What the machine needs

| Part | What tOS needs |
|---|---|
| CPU | x86-64 (64-bit). 32-bit programs run as well |
| Memory | 1 GB or more recommended |
| Boot | BIOS, through GRUB (Multiboot). The image is a bootable disc |
| Display | A VBE framebuffer such as QEMU's `-vga std`, VirtualBox and VMware: 1280x720 by default, 1024x768 to 1920x1080 in Settings. Without one, tOS falls back to a text-mode desktop |
| Input | PS/2 keyboard and mouse (virtual machines provide them) |
| Network | virtio-net, Intel E1000, Realtek RTL8139, AMD PCnet or NE2000 |
| Disk | Not needed to try tOS. An IDE, AHCI, NVMe or USB disk is needed to install it |

## What you see first

On a machine where tOS is not installed, it starts with its boot screen and then **tOS Recovery**, where you choose to install tOS or to try it. On an installed machine it shows the login window. Both are described on [Recovery and the installer](/tos/recovery).

![The boot screen](shot:boot_screen.png)

![The boot animation, from the logo to Recovery](video:boot-animation.mp4)

## Updating

An installed tOS updates itself: **Software Update** (in Settings) downloads a newer build, and tOS starts it. You can also download a newer image from the Releases page and install it again.

Packages are separate from the system: `sudo tpkg upgrade` installs newer versions of the packages you have. See [Packages](/tos/packages).

## Next

- [Recovery and the installer](/tos/recovery): installing step by step, or trying tOS without installing.
- [Quickstart](/tos/quickstart): your first ten minutes.
