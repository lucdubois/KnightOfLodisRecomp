A small update to 1.2.1. On Android you get a folder that keeps a copy of your saves, and a shader of its own for the second screen. Shaders look better and more of them work, and on a computer you can change mods from the menu. As always, the game is not included: you need your own *Tactics Ogre: The Knight of Lodis (USA)* ROM.

## Downloads

| You play on | Download |
|---|---|
| Android | `KnightOfLodisRecomp.apk` |
| Linux, Steam Deck | `KnightOfLodisRecomp-1.2.2-x86_64.AppImage` |
| macOS (Apple Silicon, macOS 15 or newer) | `KnightOfLodisRecomp-1.2.2-macos-arm64.zip` |
| Windows (64-bit, Windows 10 or newer) | `KnightOfLodisRecomp-1.2.2-windows-x64.zip` |

**On Android, install this APK over the one you have. Do not uninstall first:** uninstalling deletes your save and save states. Your save, save states, ROM and settings are kept.

On a computer, replace the old AppImage, app or folder with the new one. Your save, save states and settings are in their own folder and are kept.

## What's new

- **Sync Folder (Android).** In **Options > Enhancements > Technical > Sync Folder**, choose a folder. The game then copies your save, your save states and its logs there whenever they change. Use it to keep a backup, to move your game to another device, or to send the logs with a bug report.
  - If the folder already has saves from this game, the game asks whether to keep the folder's or its own. Either way, the other set goes to a `backup-<date>` folder inside it, so nothing is lost.
  - If you keep the folder's, the game restarts to use them.
  - If a save in the folder is changed by something else, another device for example, the game moves it to a backup folder before writing over it.
  - Sync Folder replaces the logs' Save button. The logs are still in the Files app, as in 1.2.1.
- **A shader for the second screen (Android).** On devices with a second screen, such as the AYN Thor, **Options > Enhancements > Display > Bottom Shader** picks a RetroArch preset for the bottom screen. It is chosen and adjusted separately from the top screen's.
- **Mods from the menu on a computer.** **Options > Enhancements > Mod** now lets you choose or remove a mod (an .ips, .ups or .bps patch) and restart, as on Android. Where no file dialog can open, as in the Steam Deck's Gaming Mode, it takes you to the setup screen instead.

## Fixes

- **Shaders follow RetroArch more closely.** Shaders now get the picture laid out the way RetroArch hands it to them. For example, dot-matrix and LCD presets such as gba-dot-matrix drew an uneven grid, and now draw it even.
- **Shaders with the 3D battlefield.** With a shader on and the battlefield's perspective on, some rows of the picture were doubled or missing, so slanted edges like roofs looked torn. They are now smooth.
- **More shaders load on Android.** Presets that Android's graphics drivers refused now load, among them crt-hyllian and its variants, gizmo-crt, and the cubic presets. A few phones' graphics drivers may still refuse some of them. Some presets in libretro's pack are written in a way Android cannot run at all; they still show "Failed".
- **Achievements:** after logging in from Options > Achievements, the page stayed on "Loading the game's achievements..." until the menu was closed and opened again. It now loads with the menu open.

Everything in the 1.2 notes still applies, including its known limits. Saves and save states from 1.0 to 1.2.1 work in 1.2.2.
