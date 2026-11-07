title: Installation
lede: tOS starts from a disc image. You can try it in a virtual machine in a minute, or write it to a USB stick.
---
## Get the image

Every commit on the main branch is built and published on the [Releases page](https://github.com/Artfical/tOS/releases) as **build @commit**, with `tOS.iso` attached. Take the newest one.

## Start it in QEMU

QEMU is the quickest way to try tOS and the one it is tested in the most:

```
qemu-system-x86_64 -cdrom tOS.iso -m 2048 -vga std -nic user,model=e1000
```

- `-m 2048` gives the machine 2 GB. Programs and packages live in memory, so give it more if you install a lot.
- `-vga std` is the display the desktop needs.
- `-nic user,model=e1000` gives it a network card and internet access.

## Other virtual machines

tOS also starts in VirtualBox and VMware. Create a machine for a 64-bit "Other" system, give it at least 1 GB of memory, attach `tOS.iso` as the disc, and start it. In VirtualBox, choose the **ICH AC97** audio controller to get sound.

## A USB stick

Write the image to a stick and start the computer from it. This erases the stick:

```
sudo dd if=tOS.iso of=/dev/sdX bs=4M status=progress
```

Replace `/dev/sdX` with your stick. Check the name with `lsblk` first; writing to the wrong device destroys its data. tOS is tested in virtual machines, so start with those before trying real hardware.

## Recovery: install or try


![tOS Recovery: install tOS, Disk Utility, or try tOS live](shot:recovery.png)

On a machine where tOS is not installed, it starts in **tOS Recovery**:

- **Install tOS** asks for the keyboard layout (with a picture of the keyboard), the network, an account (full name, account name, password and a picture) and the disk, then installs and restarts.
- **Disk Utility** opens the disk tool, to format or check disks first.
- **Try tOS in Live Environment** starts the desktop without installing. The user is `tos` and there is no password.

On an installed system you see the login window instead, with the account pictures, a password field, Restart and Shut Down.

![Choosing the keyboard, with a picture of it](shot:recovery_keyboard.png)

![The account: name, password and a picture to choose from](shot:recovery_account.png)

![Choosing the disk to install on](shot:recovery_disk.png)

![The login window of an installed system](shot:login.png)

## Requirements

| Part | What tOS needs |
|---|---|
| CPU | x86-64 (64-bit). 32-bit programs run too |
| Memory | 1 GB or more recommended |
| Boot | BIOS, through GRUB (the image is a bootable disc) |
| Display | A VBE framebuffer (QEMU, VirtualBox, VMware): 1280x720 by default, 1024x768 to 1920x1080 in Settings |
| Input | PS/2 keyboard and mouse |
| Network | virtio-net, E1000, RTL8139, PCnet or NE2000 |
| Disk | Not needed to start. An IDE, AHCI, NVMe or USB disk is needed to install |

## Updating

An installed tOS updates itself: Software Update downloads a newer build and starts it. You can also download a newer image from the Releases page.
