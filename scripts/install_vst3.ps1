param([string]$Configuration = "Release")
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "build\$Configuration\vst3\StoneridgeVocalChain_vst3.vst3"
if (-not (Test-Path $src)) {
    Write-Host "No VST3 bundle at $src"
    exit 0
}
$local = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3\StoneridgeVocalChain_vst3.vst3"
New-Item -ItemType Directory -Force -Path (Split-Path $local) | Out-Null
if (Test-Path $local) { Remove-Item $local -Recurse -Force }
Copy-Item $src $local -Recurse -Force
Write-Host "Installed $local"
try {
    $common = Join-Path $env:COMMONPROGRAMFILES "VST3\StoneridgeVocalChain_vst3.vst3"
    New-Item -ItemType Directory -Force -Path (Split-Path $common) -ErrorAction Stop | Out-Null
    if (Test-Path $common) { Remove-Item $common -Recurse -Force -ErrorAction Stop }
    Copy-Item $src $common -Recurse -Force -ErrorAction Stop
    Write-Host "Installed $common"
} catch {
    Write-Host "Program Files VST3 copy needs an elevated shell: $($_.Exception.Message)"
}
exit 0
