# Antigravity PostToolUse hook for write_file / edit_file fast static hygiene linting.
$ErrorActionPreference = "SilentlyContinue"

function Emit-Empty {
    [Console]::Out.WriteLine("{}")
}

$raw = [Console]::In.ReadToEnd()
if ([string]::IsNullOrWhiteSpace($raw)) {
    Emit-Empty
    exit 0
}

try {
    $payload = $raw | ConvertFrom-Json
} catch {
    Emit-Empty
    exit 0
}

$targetPath = ""
if ($payload.toolCall -and $payload.toolCall.args) {
    if ($payload.toolCall.args.Path) {
        $targetPath = [string]$payload.toolCall.args.Path
    } elseif ($payload.toolCall.args.path) {
        $targetPath = [string]$payload.toolCall.args.path
    }
}

if ([string]::IsNullOrWhiteSpace($targetPath)) {
    Emit-Empty
    exit 0
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$normRoot = $repoRoot.TrimEnd('\', '/')
$normPath = $targetPath.Replace('/', '\')
if (-not $normPath.StartsWith($normRoot + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
    Emit-Empty
    exit 0
}

$rel = $normPath.Substring($normRoot.Length + 1).Replace('\', '/')
if ($rel.StartsWith(".agents/.state/", [System.StringComparison]::OrdinalIgnoreCase)) {
    Emit-Empty
    exit 0
}

$stateDir = Join-Path $repoRoot ".agents\.state"
$stateFile = Join-Path $stateDir "hygiene_status.json"
$tmpReport = Join-Path $stateDir ("check_" + [Guid]::NewGuid().ToString("N") + ".json")
New-Item -ItemType Directory -Force -Path $stateDir | Out-Null

$checker = Join-Path $repoRoot "scripts\check_hygiene.ps1"
if (Test-Path -LiteralPath $checker) {
    $null = & $checker -Paths @($targetPath) -JsonOut $tmpReport 2>$null
}

$map = @{}
if (Test-Path -LiteralPath $stateFile) {
    try {
        $existing = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
        if ($existing -and $existing.files) {
            foreach ($prop in $existing.files.PSObject.Properties) {
                $map[$prop.Name] = @($prop.Value)
            }
        }
    } catch {
        $map = @{}
    }
}

if (Test-Path -LiteralPath $tmpReport) {
    try {
        $res = Get-Content -LiteralPath $tmpReport -Raw | ConvertFrom-Json
        if ($res -and -not $res.ok -and $res.violations) {
            $map[$rel] = @($res.violations)
        } else {
            $map.Remove($rel)
        }
    } catch {
        # Ignore malformed temp output
    }
    Remove-Item -LiteralPath $tmpReport -Force -ErrorAction SilentlyContinue
}

$stateObj = [pscustomobject]@{
    updatedUtc = [DateTime]::UtcNow.ToString("o")
    files      = $map
}
$stateObj | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $stateFile -Encoding Ascii

Emit-Empty
exit 0
