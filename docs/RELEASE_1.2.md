VS Mode and Exchange without a link cable, RetroArch shaders, sharper pixels, and an options menu that pauses the game. As always, the game is not included: you need your own *Tactics Ogre: The Knight of Lodis (USA)* ROM.

## Downloads

| You play on | Download |
|---|---|
| Android | `KnightOfLodisRecomp.apk` |
| Linux, Steam Deck | `KnightOfLodisRecomp-1.2-x86_64.AppImage` |
| macOS (Apple Silicon, macOS 15 or newer) | `KnightOfLodisRecomp-1.2-macos-arm64.zip` |
| Windows (64-bit, Windows 10 or newer) | `KnightOfLodisRecomp-1.2-windows-x64.zip` |

**On Android, install this APK over the one you have. Do not uninstall first:** uninstalling deletes your save and save states. Your save, save states, ROM and settings are kept.

On a computer, replace the old AppImage, app or folder with the new one. Your save, save states and settings are in their own folder and are kept.

## What's new everywhere

- **VS Mode against the computer.** Quest Mode's VS (the battle that needed a second GBA and a link cable) can now be played against the computer: choose **VS** at the title's Quest Mode menu with **Options > Gameplay > VS Mode** set to **Computer**. The computer fields a copy of your own army, its units and leader picked at random, and plays it with the game's own AI.
- **VS Mode and Exchange over your local network.** Set **VS Mode** to **Network** and choose **VS** or **Exchange** on two devices on the same network: they find each other on their own (**B** stops looking). Any two work together: Android, Linux, Steam Deck, Mac and Windows. Both need the same ROM, the same BIOS choice and the same mod.
- **Shaders.** **Options > Display > Shader** runs RetroArch's GLSL shader presets (`.glslp`) over the picture: CRT looks, scanlines, smoothing and more. **Download Shader Pack** fetches RetroArch's own collection (about 600 presets, 27 MB), or add your own (**Import Folder...** on Android, **Open Folder** on a computer). **Parameters** adjusts the chosen preset, **Reset** turns it off. Depth of field works with them.
- **Even pixels.** **Options > Display > Scaling**: **Integer** (the default) scales the picture by a whole number, so every pixel is the same size, with a border if needed; **Smooth** fills the screen with a light filter; **Stretch** fills it as before.
- **Options > Display > Ratio**: the picture's shape (1:1, 4:3, 3:2, 16:10 or 16:9), which sets the sizes **Graphics > View Size** offers. Until you pick one, your screen's own shape.
- **The options menu pauses the game** and no longer darkens it, on every device, the second screen of dual-screen handhelds included. A change shows on the picture at once, and the game does not move on while you try settings. A frame in the menu's own border shows that the menu has the controls.
- **Options > Technical > BIOS**: switch between the GBA BIOS and the built-in one from the menu (it applies at the next start). The licenses moved to this page too.
- **The options menu is in two pages:** the game's own options, then Enhancement Options (Preset, Display, Graphics, Audio, Gameplay, Cheats, Mod, Technical).

## Android

- **The second screen is crisp again** on dual-screen handhelds (it was slightly blurred on Android 9 and newer), and **Options > Display > Bottom Screen** chooses Integer, Smooth or Stretch for it.
- **Options > Technical > BIOS** can pick a GBA BIOS file if you did not give one at setup.

## Computers

- **Windows now uses OpenGL**, as the Mac and Linux builds do: shaders, and depth of field with the perspective, work there. If OpenGL is not available, the game falls back to Direct3D by itself.
- **Depth of field works on the Mac** (it drew nothing with the perspective on).
- **F5** saves a new numbered state and **F6** loads the latest one.
- **Window Size** also offers whole multiples of the view (2x, 3x...), which pair with Integer scaling.
- **Windows:** the first time VS or Exchange looks for a player over the network, Windows Firewall asks whether the game may use the network. Allow it on private networks.

## Known limits

- The **Windows** build has still only been tested under Wine, not on a Windows PC. Please report anything that goes wrong.
- The **macOS** build is for Apple Silicon Macs only. Its OpenGL is older: about 90% of RetroArch's shader collection runs on it (crt-royale does). One bezel preset, `uborder/.../crt-nobody-bezel-reflections`, crashes macOS's shader compiler and with it the game: avoid it on a Mac.
- **Over the network**, the options menu does not pause the game (the other player would wait), and options that change how the game runs are held until the match ends.
- The computer builds are not signed by Apple or Microsoft: see the 1.1 notes for how to open them the first time.

Saves and save states from 1.0 to 1.1.1 work in 1.2.
