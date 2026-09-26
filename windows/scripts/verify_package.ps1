# Verify the installed AgentLoom SDK with an independent Windows consumer.

[CmdletBinding()]
param(
    [string]$BuildDir = "build\x64-Release-Cold",
    [string]$Config = "Release",
    [string]$InstallDir = "build\windows-agentloom-install",
    [string]$ConsumerBuildDir = "build\windows-package-consumer",
    [string]$Generator,
    [string]$Triplet = "x64-windows-release",
    [string]$BoostRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = $PSScriptRoot
$WindowsDir = Split-Path -Parent $ScriptDir
$RepoRoot = Split-Path -Parent $WindowsDir

if (-not $BoostRoot) {
    $BoostRoot = Join-Path $RepoRoot "deps\boost_1_85_0"
}

$FullBuildDir = Join-Path $RepoRoot $BuildDir
$FullInstallDir = Join-Path $RepoRoot $InstallDir
$FullConsumerBuildDir = Join-Path $RepoRoot $ConsumerBuildDir
$PackageTest = Join-Path $RepoRoot "cmake\AgentLoomPackageConsumerTest.cmake"
$VcpkgInstalled = Join-Path $RepoRoot "vcpkg_installed"
$Toolchain = Join-Path $RepoRoot "vcpkg\scripts\buildsystems\vcpkg.cmake"

if ([string]::IsNullOrWhiteSpace($Generator)) {
    $CacheFile = Join-Path $FullBuildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $CacheFile) {
        $GeneratorLine = Get-Content -LiteralPath $CacheFile -Encoding UTF8 |
            Select-String '^CMAKE_GENERATOR:INTERNAL=' | Select-Object -First 1
        if ($GeneratorLine) {
            $Generator = ($GeneratorLine.Line -replace '^CMAKE_GENERATOR:INTERNAL=', '').Trim()
        }
    }
}
if ([string]::IsNullOrWhiteSpace($Generator)) {
    $Generator = "Visual Studio 18 2026"
}

Push-Location $RepoRoot
try {
    & cmake `
        "-DAGENTLOOM_SOURCE_DIR=$RepoRoot" `
        "-DAGENTLOOM_PACKAGE_BUILD_DIR=$FullBuildDir" `
        "-DAGENTLOOM_INSTALL_PREFIX=$FullInstallDir" `
        "-DAGENTLOOM_CONSUMER_BUILD_DIR=$FullConsumerBuildDir" `
        "-DAGENTLOOM_CONSUMER_GENERATOR=$Generator" `
        "-DAGENTLOOM_CONSUMER_BUILD_TYPE=$Config" `
        "-DAGENTLOOM_CONSUMER_TOOLCHAIN_FILE=$Toolchain" `
        "-DAGENTLOOM_CONSUMER_TRIPLET=$Triplet" `
        "-DAGENTLOOM_CONSUMER_VCPKG_INSTALLED_DIR=$VcpkgInstalled" `
        "-DAGENTLOOM_CONSUMER_BOOST_ROOT=$BoostRoot" `
        -P $PackageTest
    if ($LASTEXITCODE -ne 0) {
        throw "Package consumer verification failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
