# Antigravity Stop hook (Option A): blocks turn stop once if static hygiene violations remain.
$ErrorActionPreference = "SilentlyContinue"

function Emit-Json([hashtable]$Obj) {
    [Console]::Out.WriteLine(($Obj | ConvertTo-Json -Compress -Depth 6))
}

$raw = [Console]::In.ReadToEnd()
if ([string]::IsNullOrWhiteSpace($raw)) {
    Emit-Json @{}
    exit 0
}

try {
    $payload = $raw | ConvertFrom-Json
} catch {
    Emit-Json @{}
    exit 0
}

# Only gate normal NO_TOOL_CALL stops, and never loop when continuationActive is already true
if ($payload.terminationReason -ne "NO_TOOL_CALL" -or $payload.continuationActive -eq $true) {
    Emit-Json @{}
    exit 0
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$stateFile = Join-Path $repoRoot ".agents\.state\hygiene_status.json"
$checker = Join-Path $repoRoot "scripts\check_hygiene.ps1"

if (-not (Test-Path -LiteralPath $stateFile) -or -not (Test-Path -LiteralPath $checker)) {
    Emit-Json @{}
    exit 0
}

try {
    $state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
} catch {
    Emit-Json @{}
    exit 0
}

$flagged = @()
if ($state -and $state.files) {
    foreach ($prop in $state.files.PSObject.Properties) {
        if ($prop.Value -and @($prop.Value).Count -gt 0) {
            $flagged += $prop.Name
        }
    }
}

if ($flagged.Count -eq 0) {
    Emit-Json @{}
    exit 0
}

$tmpReport = Join-Path (Split-Path -Parent $stateFile) ("stop_" + [Guid]::NewGuid().ToString("N") + ".json")
$null = & $checker -Paths $flagged -JsonOut $tmpReport 2>$null

$activeViolations = @()
if (Test-Path -LiteralPath $tmpReport) {
    try {
        $res = Get-Content -LiteralPath $tmpReport -Raw | ConvertFrom-Json
        if ($res -and -not $res.ok -and $res.violations) {
            $activeViolations = @($res.violations)
        }
    } catch { }
    Remove-Item -LiteralPath $tmpReport -Force -ErrorAction SilentlyContinue
}

if ($activeViolations.Count -eq 0) {
    @{ updatedUtc = [DateTime]::UtcNow.ToString("o"); files = @{} } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $stateFile -Encoding Ascii
    Emit-Json @{}
    exit 0
}

$summary = ($activeViolations | Select-Object -First 8 | ForEach-Object { "[$($_.rule)] $($_.file): $($_.detail)" }) -join "; "
Emit-Json @{
    decision = "continue"
    reason   = "[Stoneridge Stop Gate] Unresolved static hygiene error(s) remain: $summary. Fix them before finishing."
}
exit 0
