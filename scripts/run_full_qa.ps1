# Stoneridge ASIO - Automated Industry-Standard QA & Verification Gate
param(
    [switch]$SkipAsan,
    [switch]$SkipAnalyze,
    [switch]$SkipHardware,
    [switch]$WithEtw
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $Root

Write-Host "=== [1/6] Release x64 Build + Full Catch2 Suite ===" -ForegroundColor Cyan
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

Write-Host "`n=== [2/6] Binary ABI & Dependency Audit (dumpbin) ===" -ForegroundColor Cyan
$dumpbin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dumpbin) {
    & $dumpbin.FullName /EXPORTS "build\Release\StoneridgeASIO_x64.dll" | Select-String "DllGetClassObject|DllCanUnloadNow|DllRegisterServer|DllUnregisterServer"
    & $dumpbin.FullName /DEPENDENTS "build\Release\StoneridgeASIO_x64.dll" | Select-String "\.dll"
}

if (-not $SkipAnalyze) {
    Write-Host "`n=== [3/6] Static Analysis Gate (/analyze /WX Zero-Warning) ===" -ForegroundColor Cyan
    cmake --preset analyze-x64
    cmake --build --preset analyze-x64 --target StoneridgeCore StoneridgeASIO
}

if (-not $SkipAsan) {
    Write-Host "`n=== [4/6] AddressSanitizer Gate (/fsanitize=address) ===" -ForegroundColor Cyan
    $asanDllDir = (Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue | Select-Object -First 1).DirectoryName
    if ($asanDllDir) { $env:PATH = "$asanDllDir;$env:PATH" }
    cmake --preset asan-x64
    cmake --build --preset asan-x64 --target StoneridgeTests
    ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure
}

if (-not $SkipHardware) {
    Write-Host "`n=== [5/6] Real COM ASIO2 Conformance + Live 6-Config Heavy-Load Stress ===" -ForegroundColor Cyan
    & ".\build\Release\AsioConformanceHost.exe"
    if ($LASTEXITCODE -ne 0) { throw "AsioConformanceHost failed!" }

    if ($WithEtw) {
        Write-Host "Starting Windows Performance Recorder (wpr -start Audio)..." -ForegroundColor DarkCyan
        & wpr.exe -start Audio
    }

    & ".\build\Release\StoneridgeTests.exe" "[.stress]" -s
    if ($LASTEXITCODE -ne 0) {
        if ($WithEtw) { & wpr.exe -cancel }
        throw "Live 6-Config Hardware Stress Suite failed!"
    }

    if ($WithEtw) {
        $etl = Join-Path $Root "build\qa_audio_trace.etl"
        & wpr.exe -stop $etl
        Write-Host "Saved ETW Audio trace to $etl" -ForegroundColor Green
    }

    Write-Host "`n=== [6/6] Head-to-Head COM Shootout: Stoneridge ASIO vs. ASIO4ALL v2 ===" -ForegroundColor Cyan
    & ".\build\Release\ShootoutBench.exe"
}

Write-Host "`n=== ALL QA GATES PASSED! ===" -ForegroundColor Green
