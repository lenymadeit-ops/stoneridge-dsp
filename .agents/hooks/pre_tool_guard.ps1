# Antigravity PreToolUse hook for run_command safety guardrails.
$ErrorActionPreference = "SilentlyContinue"

function Emit-Json([hashtable]$Obj) {
    [Console]::Out.WriteLine(($Obj | ConvertTo-Json -Compress -Depth 6))
}

$raw = [Console]::In.ReadToEnd()
if ([string]::IsNullOrWhiteSpace($raw)) {
    Emit-Json @{ decision = "allow" }
    exit 0
}

try {
    $payload = $raw | ConvertFrom-Json
} catch {
    Emit-Json @{ decision = "allow" }
    exit 0
}

$cmd = ""
if ($payload.toolCall -and $payload.toolCall.args) {
    if ($payload.toolCall.args.CommandLine) {
        $cmd = [string]$payload.toolCall.args.CommandLine
    } elseif ($payload.toolCall.args.commandLine) {
        $cmd = [string]$payload.toolCall.args.commandLine
    }
}

if ([string]::IsNullOrWhiteSpace($cmd)) {
    Emit-Json @{ decision = "allow" }
    exit 0
}

# 1. Enforce --parallel 2 on cmake --build (prevents MSVC C1060 on 8 GB laptop)
foreach ($line in ($cmd -split "`r?`n")) {
    $trimmed = $line.Trim()
    if ($trimmed.StartsWith("#")) { continue }
    if ($trimmed -match '\bcmake(?:\.exe)?\s+--build\b' -and
        $trimmed -notmatch '--parallel\s+2\b' -and
        $trimmed -notmatch '(?:^|\s)-j\s*2\b') {
        Emit-Json @{
            decision = "deny"
            reason   = "8 GB RAM rule: pass --parallel 2 to cmake --build (e.g. cmake --build build --config Release --parallel 2) to prevent MSVC C1060 heap exhaustion."
        }
        exit 0
    }
}

# 2. Block nonexistent Windows 11 WPR profile name 'AudioGlitch'
if ($cmd -match '\bwpr(?:\.exe)?\s+-start\s+AudioGlitch\b') {
    Emit-Json @{
        decision = "deny"
        reason   = "Windows 11 WPR profile 'AudioGlitch' does not exist (0x80070002). Use built-in 'Audio' profile or scripts/studio_mode.ps1 -DpcTrace."
    }
    exit 0
}

# 3. Block bare 'python' (hits Windows Store alias) when not invoked via 'uv run'
foreach ($line in ($cmd -split "`r?`n")) {
    $trimmed = $line.Trim()
    if ($trimmed.StartsWith("#")) { continue }
    if ($trimmed -match '(?:^|[;&|]\s*)python(?:\.exe)?\s+' -and $trimmed -notmatch '\buv\s+run\b') {
        Emit-Json @{
            decision = "deny"
            reason   = "Bare python hits the Windows Store alias on this machine. Use 'uv run --project tools/lab python ...' instead."
        }
        exit 0
    }
}

# 4. Block force-pushing main branch
if (($cmd -match '\bgit\s+push\b') -and
    ($cmd -match '(?:^|\s)(?:--force|-f|--force-with-lease)(?:\s|$)') -and
    ($cmd -match '\bmain\b')) {
    Emit-Json @{
        decision = "deny"
        reason   = "Force-pushing main is blocked by workspace guard."
    }
    exit 0
}

# 5. Block live ASIO / hardware stress tests while FL64.exe or StoneridgePanel.exe is open
if ($cmd -match '\[\.stress\]|\bShootoutBench(?:\.exe)?\b|\bAsioConformanceHost(?:\.exe)?\b') {
    $busy = @()
    if (Get-Process -Name "FL64" -ErrorAction SilentlyContinue) { $busy += "FL64.exe" }
    if (Get-Process -Name "StoneridgePanel" -ErrorAction SilentlyContinue) { $busy += "StoneridgePanel.exe" }
    if ($busy.Count -gt 0) {
        Emit-Json @{
            decision = "deny"
            reason   = ("Close " + ($busy -join " and ") + " before running live ASIO/stress hardware tests.")
        }
        exit 0
    }
}

Emit-Json @{ decision = "allow" }
exit 0
