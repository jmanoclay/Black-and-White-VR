# Installs (or removes) the mod in the game folder.
# Usage: powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 [-Remove] [-Config RelWithDebInfo]
param([switch]$Remove, [string]$Config = "RelWithDebInfo",
      [string]$GameDir = "D:\Games\Black & White")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if ($Remove) {
    Remove-Item "$GameDir\ddraw.dll", "$GameDir\ddraw.pdb", "$GameDir\openvr_api.dll" -ErrorAction SilentlyContinue
    Write-Host "removed the mod's DLLs from $GameDir (bwvr\ folder left in place)"
    return
}
$build = Join-Path $root "build\$Config"
Copy-Item "$build\ddraw.dll", "$build\ddraw.pdb" $GameDir -Force
Copy-Item (Join-Path $root "third_party\openvr\bin\win32\openvr_api.dll") $GameDir -Force
New-Item -ItemType Directory -Force "$GameDir\bwvr" | Out-Null
if (-not (Test-Path "$GameDir\bwvr\bwvr.ini")) {
    Copy-Item (Join-Path $root "config\bwvr.ini") "$GameDir\bwvr\bwvr.ini"
}
# SteamVR Input action manifest and default bindings (always refreshed).
New-Item -ItemType Directory -Force "$GameDir\bwvr\input" | Out-Null
Copy-Item (Join-Path $root "config\input\*.json") "$GameDir\bwvr\input" -Force
# dxwrapper (Direct3D 7 on Direct3D 9: the eyes go to SteamVR without a readback), when built by
# tools\build_dxwrapper.ps1. Used when bwvr.ini's [proxy] chain names it.
$dxw = Join-Path $root "build\dxwrapper"
if (Test-Path "$dxw\dxwrapper.dll") {
    New-Item -ItemType Directory -Force "$GameDir\bwvr\dxwrapper" | Out-Null
    Copy-Item "$dxw\dxwrapper.dll", "$dxw\License.txt" "$GameDir\bwvr\dxwrapper" -Force
    Copy-Item (Join-Path $root "config\dxwrapper.ini") "$GameDir\bwvr\dxwrapper\dxwrapper.ini" -Force
}
# Headset optics recorded by bwvr_probe, converted to the key/value form the mod reads.
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
& $py (Join-Path $root "tools\profile_to_txt.py") (Join-Path $root "profiles\index.json") "$GameDir\bwvr\hmd_profile.txt" | Out-Null
Write-Host "deployed $Config build to $GameDir"
