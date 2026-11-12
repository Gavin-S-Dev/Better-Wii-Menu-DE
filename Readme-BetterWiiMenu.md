# Better Wii Menu DE 1.3.1

Based on Dolphin 2609a. Download the Windows ZIP from the [latest release](https://github.com/Gavin-S-Dev/Better-Wii-Menu-DE/releases/latest), extract it into a writable folder, and run `BetterWiiMenuDE.exe`. Keep the EXE and all included files together.

## Setup

- Open the Wii Menu entry in your library. If system software is missing, choose a region and follow the setup prompt. Online setup requires an internet connection. Dolphin's disc-update and NAND-import options are also available in Tools.
- Add your game folders and configure a real or emulated Wii Remote in Controller Settings. Wii disc games sync to the Wii Menu while emulation is stopped.
- To start directly in the Wii Menu, right-click its library entry and enable **Automatically Launch Wii Menu on Startup**. The three-second countdown lets you cancel. Unchecking its startup option also disables future automatic launches.

## Profiles

On Windows, settings and saves are stored in `%APPDATA%\Better Wii Menu DE`. On its first normal launch, the app copies a detected stock Dolphin profile, including game folders, settings, controls, saves, and Wii data. Later changes in stock Dolphin are not automatically copied.

Use `-u <profile>` to select a different profile. For portable use, create `portable.txt` next to the EXE; the app then uses a local `User` folder. Both options bypass automatic profile import. Back up your existing profile before upgrading.

## Channels and compatibility

- The Wii Menu has 48 cells, including the Disc Channel. Installed channels share the remaining space with game channels. Overflow games remain available in the main library.
- Right-click a game to add or remove its channel. Manual removals are remembered, and temporarily unavailable drives keep their channels. Removing a custom channel preserves game saves.
- If a channel change is interrupted, stop emulation and choose **Tools > Repair Wii Menu Channels**.
- Complex icons, including Mario Party 9, retain their original artwork and animation on supported menus. Unknown menu revisions may reject oversized icons. Compatibility has been checked with Wii Menu 4.3U, Mario Kart Wii, and the USA/Asia Mario Party 9 disc; other games and regions may vary.
- Use Better Wii Menu DE to open its custom NAND. Stock Dolphin does not include this fork's expanded icon-memory support.

**Help > Check for Fork Updates** opens this project's releases when a newer version is available. Updates are downloaded through your browser.

No games or personal Wii data are included in the download.
