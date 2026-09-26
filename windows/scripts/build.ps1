# Build AgentLoom for Windows
# Mirrors linux/scripts/build.sh for Windows environment
#
# Usage:
#   .\windows\scripts\build.ps1 [-BuildDir <path>] [-Config <config>] [-Target <target>] [-Jobs <n>]
#
# Examples:
#   .\windows\scripts\build.ps1
#   .\windows\scripts\build.ps1 -Config Release -Jobs 8
#   .\windows\scripts\build.ps1 -Target agent_gateway_server

[CmdletBinding()]
param(
    [string]$BuildDir = "build\x64-Release-Cold",
    [string]$Config = "Release",
    [string]$Target,
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# === Helper Functions ===

function Write-Log {
    param([string]$Message)
    Write-Host "[build] $Message" -ForegroundColor Cyan
}

function Write-Error-Log {
    param([string]$Message)
    Write-Host "[build] error: $Message" -ForegroundColor Red
}

# === Validation ===

$ScriptDir = $PSScriptRoot
$WindowsDir = Split-Path -Parent $ScriptDir
$RepoRoot = Split-Path -Parent $WindowsDir

Write-Log "=== AgentLoom Windows Build ==="
Write-Log "Repository:   $RepoRoot"
Write-Log "Build dir:    $BuildDir"
Write-Log "Config:       $Config"
if ($Target) {
    Write-Log "Target:       $Target"
}
if ($Jobs -gt 0) {
    Write-Log "Parallel jobs: $Jobs"
}
Write-Log ""

# Check build directory
$FullBuildDir = Join-Path $RepoRoot $BuildDir
if (-not (Test-Path $FullBuildDir)) {
    Write-Error-Log "Build directory not found: $FullBuildDir"
    Write-Log "Run .\windows\scripts\configure.ps1 first"
    exit 1
}

# Check CMakeCache.txt
$CmakeCache = Join-Path $FullBuildDir "CMakeCache.txt"
if (-not (Test-Path $CmakeCache)) {
    Write-Error-Log "CMakeCache.txt not found in $FullBuildDir"
    Write-Log "Run .\windows\scripts\configure.ps1 first"
    exit 1
}

# === Build ===

Push-Location $RepoRoot
try {
    $buildArgs = @(
        "--build", $BuildDir,
        "--config", $Config
    )

    if ($Target) {
        $buildArgs += "--target", $Target
    }

    if ($Jobs -gt 0) {
        $buildArgs += "--parallel", $Jobs
    } else {
        $buildArgs += "--parallel"
    }

    if ($VerbosePreference -eq 'Continue') {
        $buildArgs += "--verbose"
    }

    Write-Log "Running: cmake $($buildArgs -join ' ')"
    Write-Log ""

    $startTime = Get-Date

    & cmake @buildArgs

    $exitCode = $LASTEXITCODE
    $endTime = Get-Date
    $duration = $endTime - $startTime

    Write-Log ""
    if ($exitCode -eq 0) {
        Write-Log "=== Build complete ==="
        Write-Log "Duration: $($duration.ToString('mm\:ss'))"
        Write-Log "Output:   $BuildDir\$Config\"
    } else {
        Write-Error-Log "Build failed with exit code $exitCode"
        Write-Log "Duration: $($duration.ToString('mm\:ss'))"
        exit $exitCode
    }
}
finally {
    Pop-Location
}
