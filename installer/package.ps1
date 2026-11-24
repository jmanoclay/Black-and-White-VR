# Makes a mod release package for the installer: bwvr-mod-<version>.zip, its .sha256, and an entry
# in releases.json, in a feed folder (default installer\out). The zip is what a GitHub release
# carries as its asset; the folder also works directly as a local feed ("folder:<path>").
#
# Usage: powershell -ExecutionPolicy Bypass -File installer\package.ps1 -Version 0.1.0 [-Notes notes.md]
#          [-Prerelease] [-Build <build folder>] [-Config Player] [-Out <feed folder>] [-WithSetup] [-WithPdb]
# Build the mod first: tools\build.ps1 -Player (the player build, without the developer features,
# in build\Player) and tools\build_dxwrapper.ps1 (build\dxwrapper). -Config RelWithDebInfo would
# package a developer build: only for tests, never a release. -WithSetup also copies the
# installer (installer\build.ps1) in as JakeypooBWVRModInstaller.exe, with installer.json (its version
# and SHA-256).
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Notes = "",
    [switch]$Prerelease,
    [string]$Build = "",
    [string]$Config = "Player",
    [string]$Out = "",
    [switch]$WithSetup,
    [switch]$WithPdb
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path -Parent $here
if (-not $Build) { $Build = Join-Path $root "build" }
# The mod's own files (settings, bindings, licences) come from the source tree that was built: the
# folder holding the build folder (a release worktree), not this script's checkout.
$src = Split-Path -Parent (Resolve-Path $Build).Path
if (-not (Test-Path (Join-Path $src "config\bwvr.ini"))) { throw "no config\bwvr.ini beside $Build`: -Build must be <source tree>\build" }
if (-not $Out) { $Out = Join-Path $here "out" }
if ($Version -notmatch '^\d+\.\d+\.\d+([-+][0-9A-Za-z.-]+)?$') { throw "version must look like 0.1.0 (or 0.1.0-beta.1)" }

$stage = Join-Path ([IO.Path]::GetTempPath()) ("bwvr-package-" + [guid]::NewGuid().ToString("N"))
$files = Join-Path $stage "files"
$defaults = Join-Path $stage "defaults"
New-Item -ItemType Directory -Force "$files\bwvr\input", "$files\bwvr\dxwrapper", "$files\bwvr\licenses", "$defaults\bwvr" | Out-Null

function Need($path) { if (-not (Test-Path $path)) { throw "missing: $path (build the mod first)" }; $path }
$bin = Join-Path $Build $Config
if ($Config -ne "Player") { Write-Warning "packaging $Config, a developer build (developer tab, tuning panel): for tests only" }
Copy-Item (Need "$bin\ddraw.dll") $files
if ($WithPdb) { Copy-Item (Need "$bin\ddraw.pdb") $files }  # 95 MB; keep the build's own for reading crash reports
Copy-Item (Need (Join-Path $src "third_party\openvr\bin\win32\openvr_api.dll")) $files
Copy-Item (Join-Path $src "config\input\*.json") "$files\bwvr\input"
Copy-Item (Join-Path $here "controls.md") "$files\bwvr\controls.md"  # the installer's Controls tab, for this version
Copy-Item (Need (Join-Path $Build "dxwrapper\dxwrapper.dll")) "$files\bwvr\dxwrapper"
Copy-Item (Join-Path $Build "dxwrapper\License.txt") "$files\bwvr\licenses\dxwrapper.txt"
Copy-Item (Join-Path $src "config\dxwrapper.ini") "$files\bwvr\dxwrapper\dxwrapper.ini"
Copy-Item (Join-Path $src "third_party\openvr\LICENSE") "$files\bwvr\licenses\openvr.txt"
Copy-Item (Join-Path $src "third_party\minhook\LICENSE.txt") "$files\bwvr\licenses\minhook.txt"
# The player's settings: installed only when they have none (never overwritten).
Copy-Item (Join-Path $src "config\bwvr.ini") "$defaults\bwvr\bwvr.ini"
# (No hmd_profile.txt: it is measured per headset; without it the mod uses SteamVR's own optics.)

$info = [ordered]@{ name = "Black & White VR"; version = $Version; date = (Get-Date -Format "yyyy-MM-dd") }
[IO.File]::WriteAllText((Join-Path $stage "bwvr-mod.json"), ($info | ConvertTo-Json))

New-Item -ItemType Directory -Force $Out | Out-Null
$zipName = "bwvr-mod-$Version.zip"
$zip = Join-Path $Out $zipName
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -Recurse -Force $stage
$sha = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
"$sha  $zipName" | Set-Content -Encoding ascii "$zip.sha256"

$setupName = $null; $setupVersion = $null; $setupSha = $null
if ($WithSetup) {
    # One name for every version: the installer updates itself, and installer.json says which
    # version this is (and its SHA-256), for installers looking for an update.
    $exe = Join-Path $here "bin\JakeypooBWVRModInstaller.exe"
    $setupVersion = (Get-Item (Need $exe)).VersionInfo.ProductVersion
    $setupName = "JakeypooBWVRModInstaller.exe"
    Copy-Item $exe (Join-Path $Out $setupName) -Force
    $setupSha = (Get-FileHash -Algorithm SHA256 (Join-Path $Out $setupName)).Hash.ToLower()
    [IO.File]::WriteAllText((Join-Path $Out "installer.json"), (([ordered]@{ version = $setupVersion; sha256 = $setupSha }) | ConvertTo-Json))
}

$notesText = ""
if ($Notes) { $notesText = [IO.File]::ReadAllText((Resolve-Path $Notes)) }
$feed = Join-Path $Out "releases.json"
$list = @()
if (Test-Path $feed) { $list = @([IO.File]::ReadAllText($feed) | ConvertFrom-Json) | Where-Object { $_.version -ne $Version } }
$entry = [pscustomobject][ordered]@{
    version = $Version; title = "Black & White VR $Version"; date = $info.date; notes = $notesText
    prerelease = [bool]$Prerelease; zip = $zipName; sha256 = $sha; size = (Get-Item $zip).Length
    setup = $setupName; setupVersion = $setupVersion; setupSha256 = $setupSha
}
$list = @($entry) + @($list)
[IO.File]::WriteAllText($feed, (ConvertTo-Json -InputObject @($list) -Depth 4))
Write-Host "packaged $zip ($sha)"
Write-Host "feed: $feed  (installer: set BWVR_FEED=folder:$Out to try it)"
