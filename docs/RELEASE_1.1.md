Knight of Lodis Recomp now runs on computers too: **Linux and the Steam Deck, macOS and Windows**, alongside Android. As always, the game is not included: you need your own *Tactics Ogre: The Knight of Lodis (USA)* ROM.

## Downloads

| You play on | Download |
|---|---|
| Android | `KnightOfLodisRecomp.apk` |
| Linux, Steam Deck | `KnightOfLodisRecomp-1.1-x86_64.AppImage` |
| macOS (Apple Silicon, macOS 15 or newer) | `KnightOfLodisRecomp-1.1-macos-arm64.zip` |
| Windows (64-bit, Windows 10 or newer) | `KnightOfLodisRecomp-1.1-windows-x64.zip` |

## Updating on Android

**Install this APK over the one you have. Do not uninstall first:** uninstalling deletes your save and save states. Installed over 1.0 or 1.0.1, the update keeps your save, your save states, your ROM and your settings.

## Getting started on a computer

- **Linux and Steam Deck:** mark the AppImage as executable and run it. On the Deck, add it to Steam (**Games > Add a Non-Steam Game**) to play it from Gaming Mode. It needs a recent system (glibc 2.40 or newer): SteamOS 3.7, Fedora 41, Ubuntu 25.04, Debian 13 or newer. Ubuntu 24.04 is too old.
- **macOS:** unzip and open **Knight of Lodis Recomp.app**. The first time, macOS blocks it because it is not notarized: open **System Settings > Privacy & Security** and choose **Open Anyway**.
- **Windows:** unzip, keep the folder together, and run `KnightOfLodisRecomp.exe`. Windows may warn that the program is from an unknown publisher: choose **More info**, then **Run anyway**.

On first start a setup window asks for your ROM, and optionally the GBA BIOS and a mod: click a box and pick the file, or drag the file onto the window. Then press **Play**.

- A controller works right away. Without one, the keyboard plays: arrows for the D-pad, **X** = A, **Z** = B, **Return** = Start, **Right Shift** = Select, **Q** / **E** = L / R. The README lists every key and how to change them.
- **Esc** (or a click of the right stick) opens the options menu. Its last row quits the game; **Alt+Return** switches full screen.
- Your save, save states and settings are kept in `~/.local/share/KnightOfLodisRecomp` (Linux), `~/Library/Application Support/KnightOfLodisRecomp` (macOS) or `%APPDATA%\KnightOfLodisRecomp` (Windows).
- **RetroAchievements:** **Options > Achievements > Account** logs you in. Type with the keyboard; on the Steam Deck, **Steam + X** shows Steam's on-screen keyboard. Achievements slide in at the top right as they unlock.
- **Options > Licenses** shows the licenses of everything the game is built with.

What the computer builds do not have yet: the second-screen pages of dual-screen handhelds (the unit sheet, the army list), touch on the menu, and choosing a mod from inside the game: a mod is chosen on the setup window, which comes back if you delete `totkol.gba` in the game's folder (your save is kept).

## What's new everywhere

- **The game's own Option window opens the options menu** (System > Option, in battle and on the world map): the game's twelve options are in it, with everything else.
- **The Save States page is a row of the options menu.** Pressing both sticks together no longer opens it.
- **A click of the right stick opens the options menu** on Android too.
- **Faster:** the lighting, shadows, roofs and lifted walls do much less work while the camera moves, so busy maps hold their frame rate better on every device.
- **Lifted Props and Lifted Walls are no longer part of the Enhanced preset:** they are the heaviest effects. They are still in **Options > Graphics**, and settings you saved are kept.

## Android

- On dual-screen handhelds, the second screen now hides while you are away from the game (Home, another app), and comes back when you return.

## Known limits

- The **Windows** build is new and has only been tested under Wine, not yet on a Windows PC. Please report anything that goes wrong.
- The **macOS** build is for Apple Silicon Macs only.
- The computer builds are not signed by Apple or Microsoft, hence the warnings above.

Saves and save states made with 1.0 and 1.0.1 work in 1.1.
