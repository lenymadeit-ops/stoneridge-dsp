# Stoneridge DSP disk audit / confirmed-cache cleanup.
# -Audit is read-only. -Clean asks once per category and never touches documents or the repo.
param(
    [switch]$Audit,
    [switch]$Clean
)

$ErrorActionPreference = "Continue"
if (-not $Audit -and -not $Clean) { $Audit = $true }

function Get-FolderBytes([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return 0 }
    $sum = 0L
    Get-ChildItem -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
        if (-not $_.PSIsContainer) { $sum += $_.Length }
    }
    return $sum
}

function Show-Gb([int64]$Bytes) {
    return ("{0:N2} GB" -f ($Bytes / 1GB))
}

$categories = @(
    @{ Name = "XDM app data"; Path = "$env:USERPROFILE\.xdm-app-data\Data"; Kind = "tree" },
    @{ Name = "McAfee program data"; Path = "C:\ProgramData\McAfee"; Kind = "tree" },
    @{ Name = "LGHUB caches"; Path = "C:\ProgramData\LGHUB"; Kind = "tree" },
    @{ Name = "Chrome cache"; Path = "$env:LOCALAPPDATA\Google\Chrome\User Data\Default\Cache"; Kind = "tree" },
    @{ Name = "Chrome code cache"; Path = "$env:LOCALAPPDATA\Google\Chrome\User Data\Default\Code Cache"; Kind = "tree" },
    @{ Name = "Waves installer cache"; Path = "$env:LOCALAPPDATA\Waves Audio"; Kind = "tree" },
    @{ Name = "NVIDIA residue"; Path = "$env:LOCALAPPDATA\NVIDIA Corporation"; Kind = "tree" },
    @{ Name = "User temp"; Path = $env:TEMP; Kind = "tree" }
)

Write-Host "Stoneridge disk audit" -ForegroundColor Cyan
$free = (Get-PSDrive -Name C).Free
Write-Host ("C: free {0}" -f (Show-Gb $free))

foreach ($c in $categories) {
    $bytes = Get-FolderBytes $c.Path
    Write-Host ("{0,-24} {1,10}  {2}" -f $c.Name, (Show-Gb $bytes), $c.Path)
}

$installerHits = @()
if (Test-Path "$env:USERPROFILE\Downloads") {
    $installerHits = Get-ChildItem "$env:USERPROFILE\Downloads" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -match '\.(exe|msi)$' -and $_.Length -gt 20MB }
    $sum = ($installerHits | Measure-Object Length -Sum).Sum
    if (-not $sum) { $sum = 0 }
    Write-Host ("{0,-24} {1,10}  {2}" -f "Large Downloads installers", (Show-Gb $sum), "$env:USERPROFILE\Downloads")
}

if (-not $Clean) {
    Write-Host "Audit only. Re-run with -Clean to confirm each category." -ForegroundColor Yellow
    exit 0
}

foreach ($c in $categories) {
    if (-not (Test-Path -LiteralPath $c.Path)) { continue }
    $answer = Read-Host "Delete cache category '$($c.Name)'? [y/N]"
    if ($answer -eq "y") {
        Get-ChildItem -LiteralPath $c.Path -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "Cleaned $($c.Name)" -ForegroundColor Green
    }
}

if ($installerHits.Count -gt 0) {
    $installerHits | ForEach-Object { Write-Host ("  installer: {0}" -f $_.Name) }
    $answer = Read-Host "Delete the listed Downloads installers? [y/N]"
    if ($answer -eq "y") {
        $installerHits | Remove-Item -Force -ErrorAction SilentlyContinue
    }
}

if (Get-Command uv -ErrorAction SilentlyContinue) { uv cache clean }
Write-Host "DISM component cleanup (may take several minutes)..." -ForegroundColor Yellow
DISM /Online /Cleanup-Image /StartComponentCleanup
$free2 = (Get-PSDrive -Name C).Free
Write-Host ("C: free after {0}" -f (Show-Gb $free2)) -ForegroundColor Green
