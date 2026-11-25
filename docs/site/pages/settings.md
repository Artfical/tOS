title: Settings
lede: System Settings changes how tOS looks and behaves: themes, icons, appearance, wallpaper, dock, display size, and updates.
icon: settings
---
Open **System Settings** from the Launcher or the dock. The sidebar on the left lists the panes, and the **Search** box at the top finds one by name.

![System Settings](shot:settings.png)

## The panes

| Pane | What it holds |
|---|---|
| **Network** | The network settings |
| **Appearance** | Themes, light or dark, the accent colour, the icon pack, window shadows |
| **Desktop & Dock** | The wallpaper, dock magnification and the open-app dots |
| **Displays** | The screen size |
| **Mouse** | The mouse settings |
| **Users & Groups** | The accounts on this machine |
| **General** | General settings |
| **Software Update** | Looks for a newer tOS build and installs it |

The account at the top of the sidebar shows your name and picture.

## Appearance

At the top, four ready-made **themes**. One click sets light or dark, the accent colour, the wallpaper and the icons together:

| Theme | Look |
|---|---|
| **tOS** | Dark, blue accent, the Dusk wallpaper, WhiteSur icons |
| **Classic** | Light, Graphite wallpaper, the old tOS icons |
| **Midnight** | Dark, purple accent, Graphite wallpaper |
| **Ocean** | Light, green accent, Ocean wallpaper |

The theme in force has an outline. Change any single setting below and no theme is outlined any more; your choices stay as you made them.

Under the themes, choose **Light** or **Dark**. The whole desktop follows it: windows, the dock, the menus and the apps. Pick an **accent colour** (blue, purple, pink, red, orange, yellow or green) for selections and buttons. **Icons** switches between **WhiteSur** (the macOS-like icons) and **Classic** (tOS's own icons from before build 577); it changes the Dock, the launcher, Files, notifications and the file, folder and disk icons at once. **Window shadows** turns the shadows under windows on or off. Everything is kept in `.desktop` in your home folder.

![Appearance, in the dark](shot:settings_accent.png)

![Appearance, in the light](shot:settings_light.png)

## Desktop & Dock

The **wallpaper**: *Dusk*, *Ocean*, *Graphite* or a *Picture* you choose. **Magnification** makes dock icons grow when the pointer passes over them. **Show indicators for open apps** puts a dot under the apps that are running.

![Desktop & Dock](shot:dock_indicators.png)

## Displays

The screen sizes the display supports: 1024x768, 1280x720 (the default), 1366x768, 1600x900 and 1920x1080. Choose one and the desktop changes at once.

![Displays, found with the Search box](shot:settings_search.png)

## How to...

| You want to... | Do this |
|---|---|
| Use the dark appearance | Settings, Appearance, choose **Dark** |
| Change the accent colour | Settings, Appearance, click a colour |
| Switch the whole look | Settings, Appearance, click a theme (tOS, Classic, Midnight, Ocean) |
| Use the old icons | Settings, Appearance, Icons, **Classic** |
| Change the wallpaper | Settings, Desktop & Dock, choose *Dusk*, *Ocean*, *Graphite* or *Picture* |
| Use one of your own pictures | In Files, open the picture in the Image Viewer and choose **Set as Wallpaper**, or pick *Picture* here |
| Hide the dots under running apps | Settings, Desktop & Dock, turn off **Show indicators for open apps** |
| Make the screen bigger | Settings, Displays, choose a larger size |
| Find a setting | Type in the **Search** box at the top of the sidebar |
| Update tOS | Settings, Software Update |

Changes take effect at once, and there is nothing to apply or restart.

## Software Update

**Software Update** checks the update server for a newer tOS build. If there is one, it downloads it and starts the new build. When an update is ready you also get a notification with **Update** and **Later**.

![A Software Update notification](shot:icons/notification.png)

Packages are updated separately, with `sudo tpkg upgrade`: see [Packages](/tos/packages).

![A short video of Settings](video:settings.mp4)
