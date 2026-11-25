# Antigravity PreInvocation hook: injects immediate alert if edited files have static hygiene violations.
$ErrorActionPreference = "SilentlyContinue"

function Emit-Json([hashtable]$Obj) {
    [Console]::Out.WriteLine(($Obj | ConvertTo-Json -Compress -Depth 8))
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

# Re-verify flagged files live so shell fixes clear stale alerts immediately
$tmpReport = Join-Path (Split-Path -Parent $stateFile) ("recheck_" + [Guid]::NewGuid().ToString("N") + ".json")
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

$lines = @("[Stoneridge Hygiene Guard] Active static hygiene violation(s) detected in recently edited files:")
foreach ($v in ($activeViolations | Select-Object -First 10)) {
    $lines += ("- [$($v.rule)] $($v.file): $($v.detail)")
}
$lines += "Fix these violations before moving on."

Emit-Json @{
    injectSteps = @(
        @{
            systemMessage = @{
                systemMessage = ($lines -join "`n")
            }
        }
    )
}
exit 0
