# Unregisters StoneridgeASIO_x64.dll and restores default USB power management
$ErrorActionPreference = "Stop"
$DllPath = Join-Path $PSScriptRoot "..\build\Release\StoneridgeASIO_x64.dll"
$DllPath = (Resolve-Path $DllPath).Path

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 0
}

Write-Host "Unregistering $DllPath..." -ForegroundColor Yellow
$regProc = Start-Process -FilePath "regsvr32.exe" -ArgumentList @("/u", "/s", "`"$DllPath`"") -Wait -PassThru
if ($regProc.ExitCode -ne 0) {
    Write-Error "regsvr32 /u failed with exit code $($regProc.ExitCode)"
}

Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_0A73&PID_002E*" -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.PSChildName -eq "Device Parameters") {
        Set-ItemProperty -Path $_.PSPath -Name "EnhancedPowerManagementEnabled" -Value 1 -Type DWord -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $_.PSPath -Name "SelectiveSuspendEnabled" -Value 1 -Type DWord -ErrorAction SilentlyContinue
    }
}

Write-Host "Unregistered 'Mackie EM-91CU+ (Stoneridge)' and restored default USB power settings." -ForegroundColor Green
