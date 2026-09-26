# Configure AgentLoom build for Windows
# Mirrors linux/scripts/configure.sh for Windows environment
#
# Usage:
#   .\windows\scripts\configure.ps1 [-BuildDir <path>] [-Tests] [-Inference] [-Media] [-LocalLlm] [-GPU]
#
# Examples:
#   .\windows\scripts\configure.ps1 -Tests
#   .\windows\scripts\configure.ps1 -BuildDir build\custom -GPU

[CmdletBinding()]
param(
    [string]$BuildDir = "build\x64-Release-Cold",
    [switch]$Tests,
    [switch]$Inference,
    [switch]$Media,
    [switch]$LocalLlm,
    [switch]$GPU,
    [string]$Triplet = "x64-windows-release",
    [string]$Generator = "Visual Studio 18 2026",
    [string]$LlamaCppRoot,
    [string]$LlamaCppBuild,
    [string]$GStreamerRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# === Configuration ===

$ScriptDir = $PSScriptRoot
$WindowsDir = Split-Path -Parent $ScriptDir
$RepoRoot = Split-Path -Parent $WindowsDir

$DepsDir = Join-Path $RepoRoot "deps"
$BuildMedia = $Media -or $Inference
$BuildLocalLlm = $LocalLlm -or $Inference

# Default llama.cpp paths
if (-not $LlamaCppRoot) {
    $LlamaCppRoot = Join-Path $DepsDir "llama.cpp"
}
if (-not $LlamaCppBuild) {
    $LlamaCppBuild = Join-Path $LlamaCppRoot "build"
}

# === Helper Functions ===

function Write-Log {
    param([string]$Message)
    Write-Host "[configure] $Message" -ForegroundColor Cyan
}

function Write-Error-Log {
    param([string]$Message)
    Write-Host "[configure] error: $Message" -ForegroundColor Red
}

function Test-CommandExists {
    param([string]$Command)
    $null -ne (Get-Command $Command -ErrorAction SilentlyContinue)
}

# === Validation ===

Write-Log "=== AgentLoom Windows Configuration ==="
Write-Log "Repository:  $RepoRoot"
Write-Log "Build dir:   $BuildDir"
Write-Log "Generator:   $Generator"
Write-Log "Triplet:     $Triplet"
Write-Log "Tests:       $Tests"
Write-Log "Inference:   $Inference"
Write-Log "Media:       $BuildMedia"
Write-Log "Local LLM:   $BuildLocalLlm"
Write-Log "GPU:         $GPU"
Write-Log ""

# Check CMake
if (-not (Test-CommandExists "cmake")) {
    Write-Error-Log "cmake not found in PATH"
    exit 1
}

$cmakeVersion = & cmake --version 2>&1 | Select-String "cmake version" | ForEach-Object { $_.ToString() }
Write-Log "CMake: $cmakeVersion"

# Check the requested generator instead of assuming a machine-specific default.
$cmakeHelp = & cmake --help 2>&1 | Out-String
if ($cmakeHelp -notmatch [regex]::Escape($Generator)) {
    Write-Error-Log "CMake does not support '$Generator' generator"
    Write-Log "For VS 2026, ensure C:\Program Files\CMake\bin\cmake.exe is before Strawberry CMake in PATH"
    exit 1
}

# Check dependencies
Write-Log "Validating dependencies..."
$validateScript = Join-Path $ScriptDir "validate_dependencies.ps1"
if (Test-Path $validateScript) {
    $validationArgs = @("-ExecutionPolicy", "Bypass", "-File", $validateScript)
    if (-not $Tests) {
        $validationArgs += "-SkipVcpkg"
    }
    if ($BuildMedia) {
        $validationArgs += "-Media"
    }
    if ($BuildLocalLlm) {
        $validationArgs += "-LocalLlm"
        $validationArgs += "-LlamaCppRoot", $LlamaCppRoot
        $validationArgs += "-LlamaCppBuild", $LlamaCppBuild
    }
    & powershell.exe @validationArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error-Log "Dependency validation failed"
        Write-Log "Run windows\scripts\prepare_deps.ps1 to download dependencies"
        exit 1
    }
} else {
    Write-Log "Skipping dependency validation (validate_dependencies.ps1 not found)"
}

Write-Log ""

# === CMake Configuration ===

$cmakeArgs = @(
    "-B", $BuildDir,
    "-G", $Generator,
    "-A", "x64",
    "-DCMAKE_CONFIGURATION_TYPES=Release"
)

# vcpkg manifest mode: point to installed packages
$VcpkgRoot = Join-Path $RepoRoot "vcpkg_installed"
$VcpkgTripletDir = Join-Path $VcpkgRoot $Triplet.Replace("-release", "")
if (Test-Path $VcpkgTripletDir) {
    $cmakeArgs += "-DCMAKE_PREFIX_PATH=$VcpkgTripletDir"
    Write-Log "Using vcpkg packages from: $VcpkgTripletDir"
} else {
    Write-Log "Warning: vcpkg triplet directory not found: $VcpkgTripletDir"
    Write-Log "Run windows\scripts\prepare_deps.ps1 to install vcpkg dependencies"
}

# vcpkg triplet
if ($Triplet) {
    $cmakeArgs += "-DBERT_VCPKG_TRIPLET=$Triplet"
}

# ONNX Runtime GPU
if ($GPU) {
    $cmakeArgs += "-DBERT_USE_ONNXRUNTIME_GPU=ON"
    Write-Log "Enabling ONNX Runtime GPU"
} else {
    $cmakeArgs += "-DBERT_USE_ONNXRUNTIME_GPU=OFF"
}

# Tests
if ($Tests) {
    $cmakeArgs += "-DBERT_BUILD_TESTS=ON"
    Write-Log "Enabling tests"
} else {
    $cmakeArgs += "-DBERT_BUILD_TESTS=OFF"
}

# Feature profile. The default is the same Core/SDK Release profile used by CI.
$cmakeArgs += "-DAGENTLOOM_BUILD_MEDIA=$(if ($BuildMedia) { 'ON' } else { 'OFF' })"
$cmakeArgs += "-DAGENTLOOM_BUILD_LOCAL_LLM=$(if ($BuildLocalLlm) { 'ON' } else { 'OFF' })"
$cmakeArgs += "-DBERT_BUILD_EMOTION_INFERENCE_SERVER=ON"
$cmakeArgs += "-DBERT_BUILD_MULTIMODAL_INFERENCE_SERVER=$(if ($Inference) { 'ON' } else { 'OFF' })"

# llama.cpp is an explicit external input for Local LLM and multimodal builds.
if ($BuildLocalLlm) {
    if (-not (Test-Path -LiteralPath $LlamaCppRoot)) {
        Write-Error-Log "llama.cpp root not found: $LlamaCppRoot"
        Write-Log "Pass -LlamaCppRoot and -LlamaCppBuild, or omit -LocalLlm/-Inference for the Core/SDK profile"
        exit 1
    }
    if (-not (Test-Path -LiteralPath $LlamaCppBuild)) {
        Write-Error-Log "llama.cpp build directory not found: $LlamaCppBuild"
        exit 1
    }
    $cmakeArgs += "-DLLAMA_CPP_ROOT=$LlamaCppRoot"
    $cmakeArgs += "-DLLAMA_CPP_BUILD=$LlamaCppBuild"
    Write-Log "llama.cpp root: $LlamaCppRoot"
    Write-Log "llama.cpp build: $LlamaCppBuild"
}

# GStreamer path: CMake can auto-detect common installations when omitted.
if ($GStreamerRoot -and -not $BuildMedia) {
    Write-Error-Log "-GStreamerRoot requires -Media or -Inference"
    exit 1
}
if ($GStreamerRoot) {
    $GStreamerProbe = Join-Path $GStreamerRoot "bin\gst-inspect-1.0.exe"
    if (-not (Test-Path $GStreamerProbe)) {
        Write-Error-Log "GStreamer root is invalid: $GStreamerProbe not found"
        exit 1
    }
    $cmakeArgs += "-DGSTREAMER_ROOT=$GStreamerRoot"
    Write-Log "GStreamer root: $GStreamerRoot"
}

# === Run CMake ===

Write-Log ""
Write-Log "Running CMake configuration..."
Write-Log "Command: cmake $($cmakeArgs -join ' ')"
Write-Log ""

Push-Location $RepoRoot
try {
    & cmake @cmakeArgs

    if ($LASTEXITCODE -ne 0) {
        Write-Error-Log "CMake configuration failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }

    Write-Log ""
    Write-Log "=== Configuration complete ==="
    Write-Log "Build directory: $BuildDir"
    Write-Log ""
    Write-Log "Next steps:"
    Write-Log "  1. Build:  cmake --build $BuildDir --config Release --parallel"
    if ($Tests) {
        Write-Log "  2. Test:   ctest --test-dir $BuildDir -C Release --output-on-failure"
    }
}
finally {
    Pop-Location
}
