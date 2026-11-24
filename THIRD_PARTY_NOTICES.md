# Third-party notices

The mod's own code is under the MIT license (`LICENSE`). It includes or builds on the following,
each under its own license, kept next to it in `third_party/`.

| Component | Where | License |
|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) (Tsuda Kageyu and contributors) | `third_party/minhook/` | BSD 2-Clause, `third_party/minhook/LICENSE.txt` |
| [OpenVR SDK](https://github.com/ValveSoftware/openvr) (Valve Corporation): headers, `openvr_api.lib` and `openvr_api.dll` | `third_party/openvr/` | BSD 3-Clause, `third_party/openvr/LICENSE` |
| [bw1-decomp](https://github.com/openblack/bw1-decomp) symbol list (openblack): the game's function names and addresses, no game code | `third_party/bw1-decomp/` | CC0 1.0, `third_party/bw1-decomp/LICENSE` |
| [dxwrapper](https://github.com/elishacloud/dxwrapper) (Elisha Riedlinger), including DDrawCompat (narzoul): not included; `tools/build_dxwrapper.ps1` downloads it, and `third_party/dxwrapper.patch` is a small change to it | `third_party/dxwrapper.patch` | zlib (dxwrapper), 0BSD (DDrawCompat) |

## Black & White

Black & White is © Lionhead Studios / Electronic Arts. This project contains none of the game's
files or code. You need your own copy of the game (v1.20 with the Fan Patch, which the installer
can apply) to use the mod. This is a fan project, not affiliated with or endorsed by Lionhead or
Electronic Arts.
