title: Activity, Network and Disk Utility
lede: The apps that show what the system is doing: running processes, the network, and the disks.
icon: taskmgr, netmon, diskmgr
---
## ![](icon:taskmgr) Activity

![Activity](shot:taskmanager.png)

**Activity** is the task manager. It lists the running processes with their state, how long the system has been up, and how much memory they use. Select a process with the mouse or the arrow keys, and end it with the **kill** button, after a confirmation. It will not end the idle task or itself.

In the Terminal, the same information is available with `ps`, `htop` (a live monitor) and `kill <pid>`; see [Terminal and shell](/tos/terminal).

## ![](icon:netmon) Network

![Network](shot:network.png)

**Network** shows your network at a glance: the addresses of the network card, and the live download and upload speed. It is a live inspector of the stack with four tabs:

| Tab | What it shows |
|---|---|
| **Interfaces** | The network card, its driver, addresses and packet counters |
| **Connections** | The open TCP, UDP and SCTP connections |
| **Routes** | The routing table |
| **Firewall** | The firewall rules |

A bar chart shows packets per second in and out as they happen. Open it with `open netmon` from the Terminal. See [Networking](/tos/networking) and [Network tools](/tos/network-tools).

## ![](icon:diskmgr) Disk Utility

![Disk Utility](shot:icons/diskutil.png)

**Disk Utility** looks at the disks of the computer. The list on the left shows each disk and its volumes (for example *QEMU HARDDISK* and *tOS Root*), and the right side shows the selected volume: its name, type, capacity, how much is used and free, its mount point, type, owners, connection and device.

The toolbar has:

| Button | What it does |
|---|---|
| **First Aid** | Checks a volume for errors and repairs what it can |
| **Partition** | Divides a disk |
| **Erase** | Erases and formats a disk |
| **Restore** | Restores a volume |
| **Unmount** | Unmounts a volume (it must not be in use) |
| **Info** | Shows the details of the selected item |

![First Aid on a volume](shot:diskutil_firstaid.png)

Every mount, unmount and format that Disk Utility does is written to a log that you can read in the Terminal with the `log` command, including a hex dump of the first sector after a format. The same operations are available in the Terminal with `disk`: see [Files, disks and users](/tos/filesystems).

> Erasing a disk destroys what is on it. Check the name of the disk twice.

Disk Utility is also on the Recovery screen, so you can prepare a disk before you install. See [Recovery and the installer](/tos/recovery).

## About tOS

**About tOS** shows the version and information about the system.

## Software Update

**Software Update** is in [Settings](/tos/settings). It checks for a newer build and installs it.

## Tips

- **First Aid** only checks and changes nothing. Run it before reporting disk errors.
- **Erase** destroys all data on the selected volume or disk. Check the name twice.
- **Partition** and **Restore** are not implemented yet.
- The `disk` command does the same things from the Terminal: see [Files, disks and users](/tos/filesystems).
