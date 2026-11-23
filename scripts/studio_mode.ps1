# Reversible studio power, MMCSS, and service profile for Stoneridge DSP.
param(
    [switch]$Audit,
    [switch]$Enable,
    [switch]$Restore,
    [switch]$DpcTrace,
    [switch]$KeepWaves
)

$ErrorActionPreference = "Stop"
if (-not $Audit -and -not $Enable -and -not $Restore -and -not $DpcTrace) { $Audit = $true }

$backupDir = Join-Path $env:LOCALAPPDATA "StoneridgeDSP"
$backupPath = Join-Path $backupDir "system_backup.json"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null

$usbSub = "2a737441-1930-4402-8d77-b2bebba308a3"
$usbSet = "48e6b7a6-50f5-4782-a5d4-53bb8f07e226"
$mmPath = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile"
$proPath = Join-Path $mmPath "Tasks\Pro Audio"
$prioPath = "HKLM:\SYSTEM\CurrentControlSet\Control\PriorityControl"
$services = @(
    "SonarworksAPOConfigService", "SonarworksWDMDriverService",
    "WavesSysSvc", "WavesLocalServer", "WavesPluginServer", "WavesSvc64",
    "SysMain", "WSearch", "DiagTrack", "DoSvc", "DellFFDPWmiService",
    "Spooler", "LGHUBUpdaterService"
)

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Read-Dword([string]$Path, [string]$Name, [int]$Fallback) {
    try { return [int](Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop).$Name } catch { return $Fallback }
}

if ($Audit -or $Enable) {
    Write-Host "USB selective suspend AC/DC, MMCSS, and service snapshot" -ForegroundColor Cyan
    powercfg /q SCHEME_CURRENT $usbSub $usbSet
    Write-Host ("NetworkThrottlingIndex={0}" -f (Read-Dword $mmPath "NetworkThrottlingIndex" -1))
    Write-Host ("SystemResponsiveness={0}" -f (Read-Dword $mmPath "SystemResponsiveness" -1))
    Write-Host ("ProAudio Priority={0}" -f (Read-Dword $proPath "Priority" -1))
    Write-Host ("Win32PrioritySeparation={0}" -f (Read-Dword $prioPath "Win32PrioritySeparation" -1))
    $os = Get-CimInstance Win32_OperatingSystem
    $compressed = (Get-Process -Name "Memory Compression" -ErrorAction SilentlyContinue | Measure-Object WorkingSet64 -Sum).Sum
    Write-Host ("FreePhysicalMB={0} MemoryCompressionMB={1}" -f ([math]::Round($os.FreePhysicalMemory / 1024, 1)), ([math]::Round(($compressed / 1MB), 1)))
}

if ($DpcTrace) {
    if (-not (Test-IsAdmin)) { Write-Error "DpcTrace requires an elevated shell."; exit 1 }
    $etl = Join-Path $backupDir "audioglitch.etl"
    wpr.exe -start AudioGlitch
    Start-Sleep -Seconds 15
    wpr.exe -stop $etl
    Write-Host "DPC trace written to $etl"
}

if (-not $Enable -and -not $Restore) { exit 0 }

if (-not (Test-IsAdmin)) {
    Write-Error "Enable and Restore require an elevated shell."
    exit 1
}

if ($Restore) {
    if (-not (Test-Path $backupPath)) { Write-Error "No backup at $backupPath"; exit 1 }
    $snap = Get-Content $backupPath -Raw | ConvertFrom-Json
    powercfg /setacvalueindex SCHEME_CURRENT $usbSub $usbSet $snap.usbAc
    powercfg /setdcvalueindex SCHEME_CURRENT $usbSub $usbSet $snap.usbDc
    powercfg /setactive SCHEME_CURRENT
    Set-ItemProperty -Path $mmPath -Name "NetworkThrottlingIndex" -Value ([int]$snap.network) -Type DWord
    Set-ItemProperty -Path $mmPath -Name "SystemResponsiveness" -Value ([int]$snap.sysResp) -Type DWord
    Set-ItemProperty -Path $proPath -Name "Priority" -Value ([int]$snap.proPri) -Type DWord
    Set-ItemProperty -Path $proPath -Name "Scheduling Category" -Value $snap.proSched -Type String
    Set-ItemProperty -Path $proPath -Name "SFIO Priority" -Value $snap.proSfio -Type String
    Set-ItemProperty -Path $prioPath -Name "Win32PrioritySeparation" -Value ([int]$snap.prioSep) -Type DWord
    foreach ($svc in $snap.services) {
        if ($svc.StartMode) { Set-Service -Name $svc.Name -StartupType $svc.StartMode -ErrorAction SilentlyContinue }
        if ($svc.Status -eq "Running") { Start-Service -Name $svc.Name -ErrorAction SilentlyContinue }
    }
    Write-Host "Studio mode restored from $backupPath" -ForegroundColor Green
    exit 0
}

$svcSnap = @()
foreach ($name in $services) {
    $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
    if (-not $svc) { continue }
    $wmi = Get-CimInstance Win32_Service -Filter "Name='$name'" -ErrorAction SilentlyContinue
    $svcSnap += @{ Name = $name; Status = [string]$svc.Status; StartMode = [string]$wmi.StartMode }
}

$snapOut = @{
    usbAc = 1; usbDc = 1
    network = (Read-Dword $mmPath "NetworkThrottlingIndex" 10)
    sysResp = (Read-Dword $mmPath "SystemResponsiveness" 20)
    proPri = (Read-Dword $proPath "Priority" 1)
    proSched = "Medium"
    proSfio = "Normal"
    prioSep = (Read-Dword $prioPath "Win32PrioritySeparation" 2)
    services = $svcSnap
}
try { $snapOut.proSched = (Get-ItemProperty $proPath)."Scheduling Category" } catch {}
try { $snapOut.proSfio = (Get-ItemProperty $proPath)."SFIO Priority" } catch {}
$snapOut | ConvertTo-Json -Depth 5 | Set-Content -Path $backupPath -Encoding ASCII

powercfg /setacvalueindex SCHEME_CURRENT $usbSub $usbSet 0
powercfg /setdcvalueindex SCHEME_CURRENT $usbSub $usbSet 0
powercfg /setactive SCHEME_CURRENT
Set-ItemProperty -Path $mmPath -Name "NetworkThrottlingIndex" -Value 0xFFFFFFFF -Type DWord
Set-ItemProperty -Path $mmPath -Name "SystemResponsiveness" -Value 10 -Type DWord
Set-ItemProperty -Path $proPath -Name "Priority" -Value 6 -Type DWord
Set-ItemProperty -Path $proPath -Name "Scheduling Category" -Value "High" -Type String
Set-ItemProperty -Path $proPath -Name "SFIO Priority" -Value "High" -Type String
Set-ItemProperty -Path $prioPath -Name "Win32PrioritySeparation" -Value 0x18 -Type DWord

foreach ($name in $services) {
    if ($KeepWaves -and $name -like "Waves*") { continue }
    $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($svc -and $svc.Status -eq "Running") { Stop-Service -Name $name -Force -ErrorAction SilentlyContinue }
}
Get-Process -Name "SoundID Reference","PhoneExperienceHost","OneDrive" -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$exclusions = @(
    "C:\Program Files\Image-Line\FL Studio 2025",
    "C:\Program Files\Common Files\VST3",
    "$env:LOCALAPPDATA\Programs\Common\VST3",
    "C:\Users\dell\Documents\Stoneridge DSP"
)
foreach ($ex in $exclusions) {
    Add-MpPreference -ExclusionPath $ex -ErrorAction SilentlyContinue
}

Write-Host "Studio mode enabled. Backup: $backupPath" -ForegroundColor Green
powercfg /q SCHEME_CURRENT $usbSub $usbSet
