# Backs up FL Studio 2025 general settings and disables background indexing / cloud search.
$ErrorActionPreference = "Stop"
$regPath = "HKCU:\Software\Image-Line\FL Studio 25\General"
$backupDir = Join-Path $env:LOCALAPPDATA "StoneridgeDSP"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
$backup = Join-Path $backupDir "fl_studio_general.reg"

if (Test-Path $regPath) {
    reg export "HKCU\Software\Image-Line\FL Studio 25\General" $backup /y | Out-Null
    New-ItemProperty -Path $regPath -Name "Browser enable auto indexing" -Value 0 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $regPath -Name "Disable FL CLOUD search" -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $regPath -Name "Silent Startup" -Value 1 -PropertyType DWord -Force | Out-Null
    Write-Host "Updated $regPath (backup $backup)" -ForegroundColor Green
} else {
    Write-Host "FL Studio 25 General key not found. Apply the checklist inside FL Studio." -ForegroundColor Yellow
}

Write-Host @"
FL Studio 2025 checklist (Options):
  Audio device: Mackie EM-91CU+ (Stoneridge)
  Priority: Highest
  Multithreaded generator processing: ON
  Multithreaded mixer processing: ON
  Smart disable: ON
  Mix in buffer switch: OFF
  Triple buffer: OFF
  Resampling: 16-point sinc while tracking, 512-point sinc on render
  Animations: Don't distract me
  Ultrasmooth: OFF
  Keep on disk for long stems
  Record Insert 1 from Mono Mic 1 (Tap A) and Insert 2 from Mono Mic 2 (Tap B/C safety)
"@
