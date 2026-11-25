# Stoneridge DSP - Unified Tiered Automated QA & Verification Gate
param(
    [ValidateSet("Fast", "Full", "Ship")]
    [string]$Mode = "Fast",
    [switch]$WithEtw,
    [switch]$CleanRootObjs
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $Root

$reportSteps = New-Object System.Collections.ArrayList
$vst3Sha256 = ""
$swTotal = [System.Diagnostics.Stopwatch]::StartNew()

function Run-Gate([string]$Name, [scriptblock]$Body) {
    Write-Host "`n=== [$Name] ===" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $rawOut = @(& $Body)
        $summary = ""
        if ($rawOut.Count -gt 0) {
            $summary = [string]$rawOut[$rawOut.Count - 1]
        }
        Write-Host ("  -> $summary") -ForegroundColor DarkGray
        $sw.Stop()
        $null = $reportSteps.Add([pscustomobject]@{
            name      = $Name
            status    = "PASS"
            seconds   = [math]::Round($sw.Elapsed.TotalSeconds, 2)
            detail    = $summary
        })
    } catch {
        $sw.Stop()
        $null = $reportSteps.Add([pscustomobject]@{
            name      = $Name
            status    = "FAIL"
            seconds   = [math]::Round($sw.Elapsed.TotalSeconds, 2)
            detail    = $_.Exception.Message
        })
        Save-QaReport "FAIL"
        throw
    }
}

function Save-QaReport([string]$OverallStatus) {
    $gitSha = (& git rev-parse --short HEAD 2>$null)
    if ($gitSha) { $gitSha = $gitSha.Trim() } else { $gitSha = "unknown" }
    $outPath = Join-Path $Root "build\qa_report.json"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
    $payload = [pscustomobject]@{
        timestampUtc  = [DateTime]::UtcNow.ToString("o")
        mode          = $Mode
        status        = $OverallStatus
        gitSha        = $gitSha
        vst3Sha256    = $vst3Sha256
        totalSeconds  = [math]::Round($swTotal.Elapsed.TotalSeconds, 2)
        steps         = $reportSteps.ToArray()
    }
    $payload | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $outPath -Encoding Ascii
}

# 0. Pre-flight environment check
Run-Gate "Pre-Flight Guard" {
    $fl = Get-Process -Name "FL64" -ErrorAction SilentlyContinue
    $panel = Get-Process -Name "StoneridgePanel" -ErrorAction SilentlyContinue
    if ($fl -or $panel) {
        throw "Close FL64.exe and StoneridgePanel.exe before running QA (prevents file/ASIO locks)."
    }
    $freeGb = [math]::Round((Get-PSDrive C).Free / 1GB, 2)
    if ($freeGb -lt 15.0) {
        Write-Warning "Low C: free space ($freeGb GB). Consider running scripts/disk_cleanup_audit.ps1."
    }
    if ($CleanRootObjs) {
        Get-ChildItem -LiteralPath $Root -Filter "*.obj" -File | Remove-Item -Force -ErrorAction SilentlyContinue
    }
    "FL64=closed Panel=closed C_free_GB=$freeGb"
}

# 1. Fast Static Hygiene (ASCII, PS1 AST, UI rules, CSS token parity, ABI freeze)
Run-Gate "Static Hygiene" {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root "scripts\check_hygiene.ps1") -All
    if ($LASTEXITCODE -ne 0) { throw "check_hygiene.ps1 failed" }
    "check_hygiene.ps1 -All passed"
}

# 2. Release x64 Build (--parallel 2) + Full CTest
Run-Gate "Release Build + CTest" {
    & cmake --build build --config Release --parallel 2
    if ($LASTEXITCODE -ne 0) { throw "Release build failed" }
    & ctest --test-dir build -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest failed" }
    "Release build + CTest 100% passed"
}

# 3. Dual VST3 Deployment SHA-256 Hash Parity Check
Run-Gate "Dual VST3 Hash Parity" {
    $srcBin = Join-Path $Root "build\Release\vst3\StoneridgeVocalChain_vst3.vst3\Contents\x86_64-win\StoneridgeVocalChain_vst3.vst3"
    if (-not (Test-Path -LiteralPath $srcBin)) {
        return "Skipped (plugin target not built)"
    }
    $locBin = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3\StoneridgeVocalChain_vst3.vst3\Contents\x86_64-win\StoneridgeVocalChain_vst3.vst3"
    $sysBin = Join-Path $env:COMMONPROGRAMFILES "VST3\StoneridgeVocalChain_vst3.vst3\Contents\x86_64-win\StoneridgeVocalChain_vst3.vst3"
    if (-not (Test-Path -LiteralPath $locBin)) { throw "Missing local VST3 install: $locBin" }
    if (-not (Test-Path -LiteralPath $sysBin)) { throw "Missing Program Files VST3 install: $sysBin" }

    $hSrc = (Get-FileHash -LiteralPath $srcBin -Algorithm SHA256).Hash
    $hLoc = (Get-FileHash -LiteralPath $locBin -Algorithm SHA256).Hash
    $hSys = (Get-FileHash -LiteralPath $sysBin -Algorithm SHA256).Hash
    if ($hSrc -ne $hLoc) { throw "Local VST3 SHA-256 mismatch ($hLoc != $hSrc). Re-run scripts/install_vst3.ps1." }
    if ($hSrc -ne $hSys) { throw "Program Files VST3 SHA-256 mismatch ($hSys != $hSrc). Re-run scripts/install_vst3.ps1." }
    $script:vst3Sha256 = $hSrc
    "SHA256=$($hSrc.Substring(0, 16)) matched build, LOCALAPPDATA, and Program Files"
}

if ($Mode -eq "Full" -or $Mode -eq "Ship") {
    # 4. Binary ABI & COM Export Audit (dumpbin)
    Run-Gate "COM Exports & Dependents (dumpbin)" {
        $dumpbin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $dumpbin) { throw "dumpbin.exe not found" }
        $dll = Join-Path $Root "build\Release\StoneridgeASIO_x64.dll"
        $exp = & $dumpbin.FullName /EXPORTS $dll | Out-String
        foreach ($sym in @("DllGetClassObject", "DllCanUnloadNow", "DllRegisterServer", "DllUnregisterServer")) {
            if ($exp -notmatch [regex]::Escape($sym)) {
                throw "Missing required COM export: $sym"
            }
        }
        $deps = & $dumpbin.FullName /DEPENDENTS $dll | Select-String "\.dll"
        "Verified 4 COM exports and $($deps.Count) DLL dependencies"
    }

    # 5. Static Analysis Gate (/W4 /analyze /WX --parallel 2, 0 warnings in Stoneridge sources)
    Run-Gate "Static Analysis (/analyze)" {
        if (-not (Test-Path -LiteralPath (Join-Path $Root "build-analyze\CMakeCache.txt"))) {
            & cmake --preset analyze-x64
            if ($LASTEXITCODE -ne 0) { throw "cmake --preset analyze-x64 failed" }
        }
        $targets = @("StoneridgeCore", "StoneridgeASIO", "StoneridgePanel", "StoneridgeTests")
        if (Test-Path -LiteralPath (Join-Path $Root "build-analyze\StoneridgeVocalChain_vst3.vcxproj")) {
            $targets += @("StoneridgeVocalChain_vst3", "StoneridgeVocalChain_clap", "StoneridgeVocalChainRt")
        }
        $buildOut = & cmake --build build-analyze --config Release --parallel 2 --target @targets 2>&1
        if ($LASTEXITCODE -ne 0) {
            $buildOut | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
            throw "build-analyze failed"
        }
        $repoEsc = [regex]::Escape($Root)
        $ownWarnings = @($buildOut | Where-Object { $_ -match $repoEsc -and $_ -match 'warning C\d+' })
        if ($ownWarnings.Count -gt 0) {
            $ownWarnings | ForEach-Object { Write-Host $_ -ForegroundColor Red }
            throw "Found $($ownWarnings.Count) /analyze warning(s) in Stoneridge sources"
        }
        "0 warnings in Stoneridge sources across $($targets.Count) target(s)"
    }

    # 6. AddressSanitizer Gate (StoneridgeTests non-benchmark + StoneridgeVocalChainRt)
    Run-Gate "AddressSanitizer (ASan)" {
        if (-not (Test-Path -LiteralPath (Join-Path $Root "build-asan\CMakeCache.txt"))) {
            & cmake --preset asan-x64
            if ($LASTEXITCODE -ne 0) { throw "cmake --preset asan-x64 failed" }
        }
        $asanTargets = @("StoneridgeTests")
        if (Test-Path -LiteralPath (Join-Path $Root "build-asan\StoneridgeVocalChainRt.vcxproj")) {
            $asanTargets += "StoneridgeVocalChainRt"
        }
        & cmake --build build-asan --config Release --parallel 2 --target @asanTargets
        if ($LASTEXITCODE -ne 0) { throw "build-asan failed" }

        $asanDllDir = (Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue | Select-Object -First 1).DirectoryName
        if ($asanDllDir) { $env:PATH = "$asanDllDir;$env:PATH" }

        & ".\build-asan\Release\StoneridgeTests.exe" "~[benchmark]"
        if ($LASTEXITCODE -ne 0) { throw "ASan StoneridgeTests failed (exit $LASTEXITCODE)" }

        if (Test-Path -LiteralPath ".\build-asan\Release\StoneridgeVocalChainRt.exe") {
            & ".\build-asan\Release\StoneridgeVocalChainRt.exe"
            if ($LASTEXITCODE -ne 0) { throw "ASan StoneridgeVocalChainRt failed (exit $LASTEXITCODE)" }
        }
        "ASan StoneridgeTests + VocalChainRt passed with 0 errors"
    }

    # 7. Plugin Validators (pluginval --strictness-level 5 on BOTH VST3 installs + clap-validator)
    Run-Gate "Plugin Validators (pluginval + clap-validator)" {
        $clapFile = Join-Path $Root "build\Release\StoneridgeVocalChain_clap.clap"
        if (-not (Test-Path -LiteralPath $clapFile)) {
            return "Skipped (plugin not built)"
        }
        $pv = if ($env:PLUGINVAL_EXE) { $env:PLUGINVAL_EXE } else { "C:\Users\dell\Documents\stoneridge-dom\.ci-tools\pluginval\pluginval.exe" }
        $cv = if ($env:CLAP_VALIDATOR_EXE) { $env:CLAP_VALIDATOR_EXE } else { "C:\Users\dell\Documents\stoneridge-dom\.ci-tools\clap-validator\clap-validator.exe" }
        if (-not (Test-Path -LiteralPath $pv)) { throw "pluginval.exe not found at $pv" }
        if (-not (Test-Path -LiteralPath $cv)) { throw "clap-validator.exe not found at $cv" }

        $locBundle = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3\StoneridgeVocalChain_vst3.vst3"
        $sysBundle = Join-Path $env:COMMONPROGRAMFILES "VST3\StoneridgeVocalChain_vst3.vst3"
        $logDir = Join-Path $Root "build\qa-logs"
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null

        foreach ($item in @(
            @{ name = "local"; path = $locBundle },
            @{ name = "system"; path = $sysBundle }
        )) {
            $outLog = Join-Path $logDir "pluginval_$($item.name).out.txt"
            $errLog = Join-Path $logDir "pluginval_$($item.name).err.txt"
            $p = Start-Process -FilePath $pv -ArgumentList @("--strictness-level", "5", "--validate", ('"' + $item.path + '"')) -Wait -PassThru -NoNewWindow -RedirectStandardOutput $outLog -RedirectStandardError $errLog
            if ($p.ExitCode -ne 0) {
                if (Test-Path $outLog) { Get-Content $outLog -Tail 25 | ForEach-Object { Write-Host $_ } }
                throw "pluginval failed on $($item.name) bundle (exit $($p.ExitCode))"
            }
        }

        & $cv validate $clapFile
        if ($LASTEXITCODE -ne 0) { throw "clap-validator failed (exit $LASTEXITCODE)" }
        "pluginval (local + Program Files) and clap-validator passed"
    }
}

if ($Mode -eq "Ship") {
    # 8. Live Hardware Conformance, Mackie EM-91CU+ Stress Suite, & Head-to-Head Shootout
    Run-Gate "Live Hardware ASIO Conformance + Mackie Stress + Shootout" {
        $mackie = Get-PnpDevice -Class AudioEndpoint -Status OK -ErrorAction SilentlyContinue | Where-Object { $_.FriendlyName -match 'EM-91CU' }
        if (-not $mackie) {
            throw "Mackie EM-91CU+ USB audio endpoint not detected. Plug it in before running -Mode Ship."
        }

        & ".\build\Release\AsioConformanceHost.exe"
        if ($LASTEXITCODE -ne 0) { throw "AsioConformanceHost failed (exit $LASTEXITCODE)" }

        if ($WithEtw) {
            Write-Host "Starting Windows Performance Recorder (wpr -start Audio)..." -ForegroundColor DarkCyan
            & wpr.exe -start Audio
            if ($LASTEXITCODE -ne 0) { throw "wpr.exe -start Audio failed (requires elevated shell)" }
        }

        & ".\build\Release\StoneridgeTests.exe" "[.stress]" -s
        if ($LASTEXITCODE -ne 0) {
            if ($WithEtw) { & wpr.exe -cancel }
            throw "Live 6-Config Hardware Stress Suite failed (exit $LASTEXITCODE)"
        }

        if ($WithEtw) {
            $etl = Join-Path $Root "build\qa_audio_trace.etl"
            & wpr.exe -stop $etl
            Write-Host "Saved ETW Audio trace to $etl" -ForegroundColor Green
        }

        & ".\build\Release\ShootoutBench.exe"
        if ($LASTEXITCODE -ne 0) { throw "ShootoutBench failed (exit $LASTEXITCODE)" }
        "AsioConformanceHost + 6-config [.stress] + ShootoutBench passed on Mackie EM-91CU+"
    }
}

Save-QaReport "PASS"
Write-Host "`n=== QA GATE ($Mode) PASSED in $([math]::Round($swTotal.Elapsed.TotalSeconds, 1))s (report: build\qa_report.json) ===" -ForegroundColor Green
exit 0
