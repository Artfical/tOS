title: The apps
lede: Everything that comes with tOS, what each app is for, and where to read more.
icon: launcher
---
The **Launcher** shows all the apps. This page explains each one briefly, and says where to read more.

![The Launcher](shot:launcher.png)

## At a glance

| App | What it is for | Read more |
|---|---|---|
| ![](icon:files) **Files** | The file manager: browse, copy, move, rename, delete, open | [Files](/tos/files-app) |
| ![](icon:terminal) **Terminal** | The shell | [Terminal and shell](/tos/terminal) |
| ![](icon:tfire) **tFire** | The web browser | [tFire](/tos/tfire) |
| ![](icon:notepad) **Notepad** | Plain text editing with line numbers and highlighting | [Notepad, Paint and viewers](/tos/productivity) |
| ![](icon:calculator) **Calculator** | A calculator with a scientific row | [Notepad, Paint and viewers](/tos/productivity) |
| ![](icon:paint) **Paint** | Drawing, with layers and zoom, saving PNG pictures | [Notepad, Paint and viewers](/tos/productivity) |
| ![](icon:media) **Music** | The media player: WAV, MP3, AAC and M4A | [Audio](/tos/audio) |
| ![](icon:netmon) **Network** | Your addresses and a live view of the network | [Activity, Network and Disk Utility](/tos/system-apps) |
| ![](icon:taskmgr) **Activity** | The running processes | [Activity, Network and Disk Utility](/tos/system-apps) |
| ![](icon:settings) **System Settings** | Appearance, desktop, display and updates | [Settings](/tos/settings) |
| ![](icon:viewer) **Image Viewer** | Opens PNG pictures | [Notepad, Paint and viewers](/tos/productivity) |
| ![](icon:pdf) **PDF Viewer** | Opens PDF files | [Notepad, Paint and viewers](/tos/productivity) |
| ![](icon:sticky) **Sticky Notes** | A small pad of notes that saves itself | [Notepad, Paint and viewers](/tos/productivity) |
| ![](icon:clock) **Clock** | An analogue and digital clock with a stopwatch | [Notepad, Paint and viewers](/tos/productivity) |
| ![](icon:diskmgr) **Disk Utility** | Look at, repair, format and grow disks | [Activity, Network and Disk Utility](/tos/system-apps) |
| ![](icon:snake) **Snake**, ![](icon:2048) **2048**, ![](icon:doom) **DOOM** | Games | [Games and demos](/tos/games) |
| ![](icon:about) **About tOS** | The version and system information | |

## Which app for what

| I want to... | Use |
|---|---|
| Browse files and folders, plug in a USB stick | [Files](/tos/files-app) |
| Type commands, install software | Terminal |
| Read a web page | [tFire](/tos/tfire) |
| Write or edit a text or a program | Notepad (or vim, nano) |
| Draw or edit a picture | Paint |
| Look at a picture or a PDF | Image Viewer, PDF Viewer |
| Keep small notes | Sticky Notes |
| Listen to music | Music |
| See what is running, or end a stuck program | Activity |
| See the network | Network |
| Check or format a disk | Disk Utility |
| Change how it looks, the screen size, update | System Settings |
| Play | Snake, 2048, DOOM |

## Starting an app

There are several ways to start an app:

- Click it in the **dock** or in the **Launcher**.
- In the Terminal, type `open <app>`: for example `open notepad`, `open files`, `open settings`, `open diskutil`. `open notepad file.txt` opens a file in an app.
- Double-click a file in **Files**: it opens in the app that fits it.
- From a script: `tos.open_app("notepad")` in MicroPython (see [MicroPython and T#](/tos/scripting)).

## Apps from packages

Linux programs that draw their own windows (through the X server) run inside a tOS window too. They are not in the Launcher; you start them from the Terminal with `tos-x`. See [Windowed programs](/tos/dev-x11).

## Where apps keep their files

Apps that save files (Notepad, Paint) use the standard file dialog, which looks like Files. Pictures from Paint are saved as real PNG files that any other program can open. Sticky Notes keeps its pages in a hidden file in the root so that they are there at the next start.

![Paint saves through the standard file dialog](shot:paint_save.png)
