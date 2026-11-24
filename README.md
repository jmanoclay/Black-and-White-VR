# Black & White VR

Play Lionhead's **Black & White** (2001) in virtual reality: your hand is the god hand, the land
is all around you. Made for SteamVR (developed on a Valve Index).

### [⬇ Download the installer (JakeypooBWVRModInstaller.exe)](https://github.com/jmanoclay/Black-and-White-VR/releases/latest/download/JakeypooBWVRModInstaller.exe)

Always the latest version. Patch notes and older versions are on [Releases](../../releases/latest).

**[🌐 Website](https://jmanoclay.github.io/Black-and-White-VR/)** · **[💬 Join the Discord](https://discord.gg/YzWQUZSsYZ)**: new releases are announced there, and it's the place to chat, ask for help and share feedback.

## Install
1. [Download **`JakeypooBWVRModInstaller.exe`**](https://github.com/jmanoclay/Black-and-White-VR/releases/latest/download/JakeypooBWVRModInstaller.exe) and run it. It keeps itself up to date.
   It isn't code-signed yet, so Windows may say *"Windows protected your PC"*: click
   **More info**, then **Run anyway**.
2. The installer finds your Black & White folder and checks it, step by step:
   - **Game patches**: the mod needs v1.20 with the Fan Patch 1.42. If your game isn't there yet,
     the installer downloads the patches from [bwgame.net](https://www.bwgame.net) (with your OK)
     and applies them. It works on Windows 11, where the old patch installers don't.
   - **SteamVR** must be installed.
   - **The VR mod**: Install, and later Update when a new version is out. The patch notes are
     shown in the installer and on each release here.
3. Press **Launch in VR**.

## What you need
- Black & White (2001), the English CD version, installed. (It isn't sold digitally.)
- Windows 10 or 11, SteamVR and a PC VR headset.

## Your files are safe
Before changing anything, the installer zips your saves and copies every game file it is about to
change into a **BWVR Backups** folder in the game folder, with one-click restore. Your VR
settings are never overwritten. Uninstall removes exactly the mod's files and keeps the game
patched.

## Source code
The mod is open source under the [MIT license](LICENSE). You don't need any of it to play; it's
here for anyone who wants to read, build or change the mod.

| Folder | What |
|---|---|
| `src/` | The mod: a `ddraw.dll` proxy (C++, 32-bit) that the game loads. It hooks the engine (MinHook), draws each eye with the game's own 3D engine, and drives SteamVR through OpenVR. `core/` start-up, settings, logging and audio, `render/` the stereo rendering, `vr/` OpenVR, `control/` the hands, `game/` the game's own structures, `ui/` the in-headset menus, `proxy/` the DLL proxy. |
| `config/` | `bwvr.ini` (the mod's settings, copied to the game's `bwvr\` folder) and the SteamVR Input action manifest and controller bindings. |
| `installer/` | The installer and launcher (C#, .NET Framework 4.8): finds the game, patches it to v1.20 + Fan Patch, installs and updates the mod. See [documentation/installer.md](documentation/installer.md). |
| `tools/` | Build, deploy and test scripts. `tools/harness/` drives the running game from Python; `tools/validate/` holds the automated tests built on it; `tools/perf/` and `tools/analysis/` are measuring tools. |
| `third_party/` | MinHook, the OpenVR SDK, the openblack symbol list, and the patch to dxwrapper. |
| `documentation/` | How things work: [controls](documentation/controls.md), [miracles in VR](documentation/miracles.md), [the VR interaction design](documentation/vr-interaction-design.md), [performance](documentation/performance.md), [the game's render passes](documentation/render-pass-map.md), [the installer](documentation/installer.md). |
| `docs/` | The website. |

### Building
Windows, with Visual Studio 2022 (or the Build Tools) with the C++ x86/x64 tools, CMake
(`C:\Program Files\CMake`), Ninja (`winget install Ninja-build.Ninja`) and Python 3.12 for the
tools and tests.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1             # the mod, build\RelWithDebInfo\ddraw.dll
powershell -ExecutionPolicy Bypass -File tools\build_dxwrapper.ps1   # dxwrapper with the mod's patch (cloned next to this repo)
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -GameDir "D:\Games\Black & White"
powershell -ExecutionPolicy Bypass -File installer\build.ps1         # the installer, installer\bin\
```

`tools\build.ps1 -Player` builds without the developer features, as the releases are built.

Installers from 1.0.14 on ask the player once whether to count their launches (an anonymous
random id and the mod's version, nothing else); `installer/stats/` has the Cloudflare Worker that
counts them. If you build your own installer, point `BWVR_STATS_URL` at your own counter.

## Credits
Black & White © Lionhead Studios / Electronic Arts; this is an unofficial fan project and contains
none of the game's files or code. Fan Patch 1.42 by Matt Stevens (bwfiles.com), hosted by
bwgame.net. The mod uses OpenVR (Valve), MinHook, dxwrapper and the openblack project's symbol
list; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Mod by Jakeypoo, MIT license.
