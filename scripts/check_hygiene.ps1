# Stoneridge DSP - Canonical Fast Static Hygiene & Invariant Checker
param(
    [string[]]$Paths,
    [switch]$Staged,
    [switch]$All,
    [switch]$AllowAbiChange,
    [string]$JsonOut
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $Paths -and -not $Staged -and -not $All) {
    $All = $true
}

function Get-RelativeRepoPath([string]$FullPath) {
    $normRoot = $RepoRoot.TrimEnd('\', '/')
    $normPath = $FullPath.Replace('/', '\')
    if ($normPath.StartsWith($normRoot + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
        return $normPath.Substring($normRoot.Length + 1).Replace('\', '/')
    }
    return $normPath.Replace('\', '/')
}

$violations = New-Object System.Collections.ArrayList

function Add-Violation([string]$File, [string]$Rule, [string]$Detail) {
    $null = $violations.Add([pscustomobject]@{
        file   = $File
        rule   = $Rule
        detail = $Detail
    })
}

$targetFiles = New-Object System.Collections.ArrayList

if ($Paths -and $Paths.Count -gt 0) {
    foreach ($p in $Paths) {
        if ([string]::IsNullOrWhiteSpace($p)) { continue }
        $resolved = $p
        if (-not [System.IO.Path]::IsPathRooted($resolved)) {
            $resolved = Join-Path $RepoRoot $resolved
        }
        if (Test-Path -LiteralPath $resolved -PathType Leaf) {
            $null = $targetFiles.Add((Resolve-Path -LiteralPath $resolved).Path)
        }
    }
} elseif ($Staged) {
    Push-Location $RepoRoot
    try {
        $stagedRaw = & git diff --cached --name-only --diff-filter=ACMR 2>$null
        foreach ($rel in $stagedRaw) {
            if ([string]::IsNullOrWhiteSpace($rel)) { continue }
            $normRel = $rel.Trim().Replace('\', '/')
            if ($normRel -match '^(?:\.cursor/|build/|build-[^/]+/|\.agents/\.state/)' -or
                $normRel -match '\.(obj|pdb|ilk|exe|dll|wav|m4a|etl)$') {
                Add-Violation $normRel "forbidden-staged-artifact" "Do not commit scratch, build, binary, or IDE artifacts."
                continue
            }
            $full = Join-Path $RepoRoot ($normRel.Replace('/', '\'))
            if (Test-Path -LiteralPath $full -PathType Leaf) {
                $null = $targetFiles.Add((Resolve-Path -LiteralPath $full).Path)
            }
        }
    } finally {
        Pop-Location
    }
} elseif ($All) {
    $scanDirs = @("src", "plugin", "tests", "scripts", ".agents")
    foreach ($d in $scanDirs) {
        $dirPath = Join-Path $RepoRoot $d
        if (-not (Test-Path -LiteralPath $dirPath)) { continue }
        Get-ChildItem -LiteralPath $dirPath -Recurse -File | ForEach-Object {
            $rel = Get-RelativeRepoPath $_.FullName
            if ($rel -notmatch '^\.agents/\.state/') {
                $null = $targetFiles.Add($_.FullName)
            }
        }
    }
}

$asciiRoots = @("src/", "plugin/", "tests/", "scripts/", ".agents/")
$uiRoots = @("src/ui/web/", "plugin/ui/")
$tokensTouched = $false

foreach ($full in $targetFiles) {
    $rel = Get-RelativeRepoPath $full
    $ext = [System.IO.Path]::GetExtension($full).ToLowerInvariant()

    # 1. Strict 7-bit ASCII & zero UTF-8 BOM across code, UI, tests, scripts, hooks
    $inAsciiScope = $false
    foreach ($prefix in $asciiRoots) {
        if ($rel.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $inAsciiScope = $true
            break
        }
    }
    if ($inAsciiScope) {
        $bytes = [System.IO.File]::ReadAllBytes($full)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            Add-Violation $rel "utf8-bom" "UTF-8 BOM detected; save as plain 7-bit ASCII without BOM."
        }
        $badCount = 0
        $firstBadOffset = -1
        for ($i = 0; $i -lt $bytes.Length; $i++) {
            if ($bytes[$i] -gt 127) {
                if ($firstBadOffset -lt 0) { $firstBadOffset = $i }
                $badCount++
            }
        }
        if ($badCount -gt 0) {
            Add-Violation $rel "non-ascii" "Found $badCount non-ASCII byte(s) (first at byte offset $firstBadOffset). Use plain ASCII."
        }
    }

    # 2. PowerShell AST syntax validation
    if ($ext -eq ".ps1") {
        $tokens = $null
        $parseErrors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($full, [ref]$tokens, [ref]$parseErrors)
        if ($parseErrors -and $parseErrors.Count -gt 0) {
            foreach ($pe in $parseErrors) {
                $lineNum = $pe.Extent.StartLineNumber
                Add-Violation $rel "ps1-syntax" "Line ${lineNum}: $($pe.Message)"
            }
        }
    }

    # 3. WebView2 UI rules (no backdrop-filter: blur, no shadowBlur, no raw <select>)
    $inUiScope = $false
    foreach ($prefix in $uiRoots) {
        if ($rel.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            $inUiScope = $true
            break
        }
    }
    if ($inUiScope) {
        $text = [System.IO.File]::ReadAllText($full)
        if ($ext -eq ".css" -and $text -match 'backdrop-filter\s*:\s*blur\(') {
            Add-Violation $rel "ui-backdrop-blur" "Forbidden backdrop-filter: blur() in WebView2 CSS (causes idle GPU/CPU load)."
        }
        if ($ext -eq ".js" -and $text -match '\.shadowBlur\s*=\s*(?!0(?:\.0+)?\s*[;,)])') {
            Add-Violation $rel "ui-canvas-shadowblur" "Forbidden non-zero Canvas2D shadowBlur in WebView2 JS."
        }
        if ($ext -eq ".html" -and $text -match '<select\b') {
            Add-Violation $rel "ui-native-select" "Forbidden native <select> element in WebView2 HTML; use custom Stoneridge controls."
        }
        if ($rel.EndsWith("stoneridge-tokens.css", [System.StringComparison]::OrdinalIgnoreCase)) {
            $tokensTouched = $true
        }
    }

    # 4. Plugin ABI freeze check (SONORE_NUM_PARAMS must remain 9)
    if (-not $AllowAbiChange -and $rel.Equals("plugin/VocalChainPlugin.cpp", [System.StringComparison]::OrdinalIgnoreCase)) {
        $text = [System.IO.File]::ReadAllText($full)
        if ($text -notmatch '#define\s+SONORE_NUM_PARAMS\s+9\b') {
            Add-Violation $rel "abi-freeze" "SONORE_NUM_PARAMS must stay 9 unless -AllowAbiChange is explicitly authorized."
        }
    }

    # 5. JSON parse validation for .agents/*.json and CMakePresets.json
    if ($ext -eq ".json" -and ($rel.StartsWith(".agents/", [System.StringComparison]::OrdinalIgnoreCase) -or $rel -eq "CMakePresets.json")) {
        try {
            $null = [System.IO.File]::ReadAllText($full) | ConvertFrom-Json
        } catch {
            Add-Violation $rel "json-syntax" "Invalid JSON: $($_.Exception.Message)"
        }
    }
}

# 6. Shared CSS token parity check (panel UI vs plugin UI)
if ($All -or $tokensTouched) {
    $panelTokens = Join-Path $RepoRoot "src\ui\web\stoneridge-tokens.css"
    $pluginTokens = Join-Path $RepoRoot "plugin\ui\stoneridge-tokens.css"
    if ((Test-Path -LiteralPath $panelTokens) -and (Test-Path -LiteralPath $pluginTokens)) {
        $h1 = (Get-FileHash -LiteralPath $panelTokens -Algorithm SHA256).Hash
        $h2 = (Get-FileHash -LiteralPath $pluginTokens -Algorithm SHA256).Hash
        if ($h1 -ne $h2) {
            Add-Violation "plugin/ui/stoneridge-tokens.css" "css-token-drift" "Out of sync with src/ui/web/stoneridge-tokens.css (SHA-256 mismatch)."
        }
    }
}

$report = [pscustomobject]@{
    ok         = ($violations.Count -eq 0)
    checked    = $targetFiles.Count
    count      = $violations.Count
    violations = $violations.ToArray()
}

if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $outDir = Split-Path -Parent $JsonOut
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) {
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $JsonOut -Encoding Ascii
}

if ($violations.Count -gt 0) {
    Write-Output ("HYGIENE_FAILED checked=$($targetFiles.Count) violations=$($violations.Count)")
    foreach ($v in $violations) {
        Write-Output ("  [$($v.rule)] $($v.file): $($v.detail)")
    }
    exit 1
}

Write-Output ("HYGIENE_OK checked=$($targetFiles.Count) violations=0")
exit 0
