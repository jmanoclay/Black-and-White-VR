# Build, deploy, boot to gameplay, optionally trace N frames, and print the mod's measurements.
# Stops before deploying if the build fails.
# Usage: powershell -ExecutionPolicy Bypass -File tools\run_test.ps1 [-Trace 2] [-Settle 10]
param([int]$Trace = 0, [int]$Settle = 10)
$root = Split-Path -Parent $PSScriptRoot
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
$ctl = Join-Path $root "tools\harness\bwctl.py"

& $py $ctl kill | Out-Null
Start-Sleep 1
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\build.ps1") 2>&1 |
    Where-Object { $_ -match 'error|warning C|built:' }
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED - not deploying"; exit 1 }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\deploy.ps1")
& $py $ctl boot
Start-Sleep -Seconds $Settle
if ($Trace -gt 0) { & $py $ctl capture $Trace | Out-Null; Start-Sleep -Seconds 3 }
Get-Content "D:\Games\Black & White\bwvr\logs\bwvr.log" |
    Select-String 'FATAL|failed|GGame::|fps,' | Select-Object -Last 8 | ForEach-Object { $_.Line }
