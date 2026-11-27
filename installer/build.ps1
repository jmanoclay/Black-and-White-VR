# Builds the installer/launcher: installer\bin\JakeypooBWVRModInstaller.exe, one 32-bit .NET Framework 4.8 exe
# (32-bit because Lionhead's patch engine, patchw32.dll, is a 32-bit DLL). Needs only the C#
# compiler from Visual Studio or its Build Tools (already needed for the mod); players need nothing,
# .NET Framework 4.8 is part of Windows 10 and 11.
# controls.md (the VR controls, shown in the Controls tab) is built in, and goes into each mod package.
# The version is in src\Program.cs (AssemblyVersion). The icon: res\app.ico, drawn by res\make_icon.py.
# Usage: powershell -ExecutionPolicy Bypass -File installer\build.ps1
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$csc = & $vswhere -products * -latest -find "MSBuild\**\Roslyn\csc.exe" | Select-Object -First 1
if (-not $csc) { throw "Roslyn csc.exe not found (install Visual Studio or its Build Tools)" }
$fw = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319"
$out = Join-Path $here "bin"
New-Item -ItemType Directory -Force $out | Out-Null
$refs = "System.dll","System.Core.dll","System.Drawing.dll","System.Windows.Forms.dll","System.IO.Compression.dll",
        "System.IO.Compression.FileSystem.dll","System.Web.Extensions.dll","Microsoft.VisualBasic.dll" | ForEach-Object { "/r:$fw\$_" }
# Pictures for the VR features tab (res\features\*.jpg, *.png), built in as BwVrSetup.img.<name>.
$imgs = Get-ChildItem (Join-Path $here "res\features") -Include *.jpg,*.png -Recurse -ErrorAction SilentlyContinue | ForEach-Object { "/resource:$($_.FullName),BwVrSetup.img.$($_.Name)" }
$src = Get-ChildItem (Join-Path $here "src") -Filter *.cs -Recurse | ForEach-Object { $_.FullName }
& $csc /nologo /noconfig /target:winexe /platform:x86 /optimize+ /langversion:7.3 /utf8output /warnaserror+ `
    "/out:$out\JakeypooBWVRModInstaller.exe" "/win32manifest:$here\res\app.manifest" "/win32icon:$here\res\app.ico" "/resource:$here\controls.md,BwVrSetup.controls.md" "/resource:$here\features.md,BwVrSetup.features.md" @refs @imgs @src
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built: $out\JakeypooBWVRModInstaller.exe"
