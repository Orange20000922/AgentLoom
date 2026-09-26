# Test AgentLoom on Windows
# Mirrors linux/scripts/test.sh for Windows environment
#
# Usage:
#   .\windows\scripts\test.ps1 [-BuildDir <path>] [-Config <config>] [-Label <label>] [-Exclude <regex>] [-ExcludeLabel <label>] [-SkipPackage]
#
# Examples:
#   .\windows\scripts\test.ps1
#   .\windows\scripts\test.ps1 -Label core
#   .\windows\scripts\test.ps1 -Exclude "redis|media"

[CmdletBinding()]
param(
    [string]$BuildDir = "build\x64-Release-Cold",
    [string]$Config = "Release",
    [string]$Label,
    [string]$Exclude,
    [string]$ExcludeLabel,
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# === Helper Functions ===

function Write-Log {
    param([string]$Message)
    Write-Host "[test] $Message" -ForegroundColor Cyan
}

function Write-Error-Log {
    param([string]$Message)
    Write-Host "[test] error: $Message" -ForegroundColor Red
}

# === Validation ===

$ScriptDir = $PSScriptRoot
$WindowsDir = Split-Path -Parent $ScriptDir
$RepoRoot = Split-Path -Parent $WindowsDir

Write-Log "=== AgentLoom Windows Test ==="
Write-Log "Repository: $RepoRoot"
Write-Log "Build dir:  $BuildDir"
Write-Log "Config:     $Config"
if ($Label) {
    Write-Log "Label:      $Label"
}
if ($Exclude) {
    Write-Log "Exclude:    $Exclude"
}
if ($ExcludeLabel) {
    Write-Log "Exclude label: $ExcludeLabel"
}
Write-Log ""

# Check build directory
$FullBuildDir = Join-Path $RepoRoot $BuildDir
if (-not (Test-Path $FullBuildDir)) {
    Write-Error-Log "Build directory not found: $FullBuildDir"
    exit 1
}

# === Run Tests ===

Push-Location $RepoRoot
try {
    $ctestArgs = @(
        "--test-dir", $BuildDir,
        "-C", $Config,
        "--output-on-failure"
    )

    if ($Label) {
        $ctestArgs += "-L", $Label
    }

    if ($Exclude) {
        $ctestArgs += "-E", $Exclude
    }

    if ($ExcludeLabel) {
        $ctestArgs += "-LE", $ExcludeLabel
    }

    if ($VerbosePreference -eq 'Continue') {
        $ctestArgs += "--verbose"
    }

    Write-Log "Running: ctest $($ctestArgs -join ' ')"
    Write-Log ""

    $startTime = Get-Date

    & ctest @ctestArgs

    $exitCode = $LASTEXITCODE
    $endTime = Get-Date
    $duration = $endTime - $startTime

    Write-Log ""
    if ($exitCode -eq 0) {
        Write-Log "=== All tests passed ==="
        Write-Log "Duration: $($duration.ToString('mm\:ss'))"
        if (-not $SkipPackage) {
            Write-Log "Verifying SDK package consumer..."
            $verifyScript = Join-Path $ScriptDir "verify_package.ps1"
            & powershell.exe -ExecutionPolicy Bypass -File $verifyScript `
                -BuildDir $BuildDir -Config $Config
            if ($LASTEXITCODE -ne 0) {
                Write-Error-Log "SDK package consumer verification failed with exit code $LASTEXITCODE"
                exit $LASTEXITCODE
            }
        }
    } else {
        Write-Error-Log "Tests failed with exit code $exitCode"
        Write-Log "Duration: $($duration.ToString('mm\:ss'))"
        exit $exitCode
    }
}
finally {
    Pop-Location
}
