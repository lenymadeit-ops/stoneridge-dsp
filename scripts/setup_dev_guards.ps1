# Stoneridge DSP - Configure local Git hooks and verify Antigravity guards
$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $RepoRoot
try {
    & git config core.hooksPath scripts/git-hooks
    $activePath = (& git config --get core.hooksPath).Trim()
    if ($activePath -ne "scripts/git-hooks") {
        throw "Expected git core.hooksPath='scripts/git-hooks', got '$activePath'"
    }
    Write-Host "Configured git core.hooksPath = $activePath" -ForegroundColor Green

    $hooksJson = Join-Path $RepoRoot ".agents\hooks.json"
    if (-not (Test-Path -LiteralPath $hooksJson)) {
        throw "Missing .agents/hooks.json"
    }
    $null = Get-Content -LiteralPath $hooksJson -Raw | ConvertFrom-Json
    Write-Host "Validated .agents/hooks.json" -ForegroundColor Green

    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "scripts\check_hygiene.ps1") -All
    if ($LASTEXITCODE -ne 0) {
        throw "check_hygiene.ps1 -All failed"
    }
    Write-Host "All local developer guards are active and verified." -ForegroundColor Green
} finally {
    Pop-Location
}
