# Builds dxwrapper (Direct3D 7 on Direct3D 9: the eyes go to SteamVR without a CPU readback) with
# the mod's patch, into build\dxwrapper\dxwrapper.dll. See documentation/performance.md.
#   - the source: github.com/elishacloud/dxwrapper at a fixed release tag, cloned next to this repo
#     (..\dxwrapper), with its pinned submodules;
#   - third_party\dxwrapper.patch: an export for the mod (DxwGetD3D9Surface) and no busy-wait
#     mitigation.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_dxwrapper.ps1
# (Not "Stop": Windows PowerShell turns git's messages on stderr into errors. Exit codes are checked.)
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$tag = "v1.8.8600.25"
$src = Join-Path (Split-Path -Parent $root) "dxwrapper"
$patch = Join-Path $root "third_party\dxwrapper.patch"

if (-not (Test-Path $src)) {
    git clone --depth 1 --branch $tag https://github.com/elishacloud/dxwrapper.git $src
}
Push-Location $src
try {
    $have = (git describe --tags --exact-match 2>$null)
    if ($have -ne $tag) {
        git fetch --depth 1 origin tag $tag
        git checkout -q $tag
    }
    git submodule update --init --depth 1 External/MemoryModule External/d3d8to9 External/detours External/Logging External/Hooking `
        External/dinputto8 External/imgui External/Hooking.Patterns External/DirectXMath
    # The patch, unless it's already in.
    git apply --check $patch 2>$null
    if ($LASTEXITCODE -eq 0) { git apply $patch; Write-Host "applied $patch" }
    else {
        git apply --check --reverse $patch 2>$null
        if ($LASTEXITCODE -ne 0) { throw "third_party\dxwrapper.patch neither applies nor is applied: check $src" }
    }
    # Its pre-build step writes these with cmd tricks that fail outside Visual Studio: written here,
    # and the pre/post-build steps skipped.
    Set-Content -Path "d3d8\BuildNo.rc" -Value "#define  FILEVERSION 1.15.1.0" -Encoding ascii
    if (-not (Test-Path "Dllmain\BuildNo.rc")) { Set-Content -Path "Dllmain\BuildNo.rc" -Value "#define BUILD_NUMBER 8600" -Encoding ascii }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $msbuild = & $vswhere -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    & $msbuild MakeShader\MakeShader.vcxproj "/p:SolutionDir=$src\" /p:Configuration=Release /p:Platform=Win32 /m /v:minimal /nologo  # (compiles its shaders)
    & $msbuild dxwrapper.vcxproj /p:Configuration=Release /p:Platform=Win32 /p:PreBuildEventUseInBuild=false `
        /p:PostBuildEventUseInBuild=false /m /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { throw "dxwrapper build failed" }
} finally {
    Pop-Location
}
$out = Join-Path $root "build\dxwrapper"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item (Join-Path $src "bin\Release\dxwrapper.dll"), (Join-Path $src "License.txt") $out -Force
Write-Host "built: $out\dxwrapper.dll"
