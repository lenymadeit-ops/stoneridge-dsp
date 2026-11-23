# Registers StoneridgeASIO_x64.dll for FL Studio and disables USB selective suspend on Mackie EM-91CU+
$ErrorActionPreference = "Stop"
$DllPath = Join-Path $PSScriptRoot "..\build\Release\StoneridgeASIO_x64.dll"
$DllPath = (Resolve-Path $DllPath).Path

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit 0
}

Write-Host "Registering $DllPath..." -ForegroundColor Cyan
$regProc = Start-Process -FilePath "regsvr32.exe" -ArgumentList @("/s", "`"$DllPath`"") -Wait -PassThru
if ($regProc.ExitCode -ne 0) {
    Write-Error "regsvr32 failed with exit code $($regProc.ExitCode)"
}

# Disable USB Enhanced Power Management / Selective Suspend on Mackie EM-91CU+ (VID_0A73&PID_002E)
Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_0A73&PID_002E*" -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.PSChildName -eq "Device Parameters") {
        Set-ItemProperty -Path $_.PSPath -Name "EnhancedPowerManagementEnabled" -Value 0 -Type DWord -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $_.PSPath -Name "SelectiveSuspendEnabled" -Value 0 -Type DWord -ErrorAction SilentlyContinue
        Write-Host "Disabled USB Selective Suspend on $($_.PSParentPath)" -ForegroundColor DarkCyan
    }
}

Write-Host "Success! 'Mackie EM-91CU+ (Stoneridge)' is registered and USB power-tuned for low-latency FL Studio use." -ForegroundColor Green
