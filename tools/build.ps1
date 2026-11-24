# Configures and builds the mod with the 32-bit MSVC toolchain.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Config RelWithDebInfo|Debug|Release] [-Player]
# -Player: a build for players, without the developer features (the pause menu's Developer tab, the
# left hand's tuning panel: BWVR_DEVELOPER off), in build\Player (RelWithDebInfo). The installer's
# package.ps1 takes it with -Config Player.
param([string]$Config = "RelWithDebInfo", [switch]$Player)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vsInstaller = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$vsInstaller\vswhere.exe" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvarsall.bat"
$cmake = "C:\Program Files\CMake\bin\cmake.exe"
$ninja = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
$build = Join-Path $root "build\$Config"
$dev = "ON"
if ($Player) {
    $build = Join-Path $root "build\Player"
    $Config = "RelWithDebInfo"
    $dev = "OFF"
}

$cmd = "set PATH=$vsInstaller;$(Split-Path $ninja.FullName);%PATH% && call `"$vcvars`" x86 >nul && " +
       "`"$cmake`" -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DBWVR_DEVELOPER=$dev >nul && " +
       "`"$cmake`" --build `"$build`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "built: $build"
