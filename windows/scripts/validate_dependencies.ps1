# Validate Windows dependencies for AgentLoom
# Checks that all required dependencies are present and properly structured
#
# Usage:
#   .\windows\scripts\validate_dependencies.ps1 [-Detailed] [-SkipVcpkg] [-Media] [-LocalLlm]
#
# Returns exit code 0 if all dependencies are valid, non-zero otherwise

[CmdletBinding()]
param(
    [switch]$Detailed,
    [switch]$SkipVcpkg,
    [switch]$Media,
    [switch]$LocalLlm,
    [string]$LlamaCppRoot,
    [string]$LlamaCppBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# === Configuration ===

$ScriptDir = $PSScriptRoot
$WindowsDir = Split-Path -Parent $ScriptDir
$RepoRoot = Split-Path -Parent $WindowsDir
$DepsDir = Join-Path $RepoRoot "deps"
if (-not $LlamaCppRoot) {
    $LlamaCppRoot = Join-Path $DepsDir "llama.cpp"
}
if (-not $LlamaCppBuild) {
    $LlamaCppBuild = Join-Path $LlamaCppRoot "build"
}

# === Validation State ===

$script:AllValid = $true
$script:MissingDeps = @()
$script:InvalidDeps = @()

function Write-Pass {
    param([string]$Message)
    Write-Host "  [PASS] $Message" -ForegroundColor Green
}

function Write-Fail {
    param([string]$Message)
    Write-Host "  [FAIL] $Message" -ForegroundColor Red
    $script:AllValid = $false
}

function Write-Warn {
    param([string]$Message)
    Write-Host "  [WARN] $Message" -ForegroundColor Yellow
}

function Write-Info {
    param([string]$Message)
    Write-Host "  [INFO] $Message" -ForegroundColor Cyan
}

function Test-DependencyPath {
    param(
        [string]$Path,
        [string]$Name,
        [switch]$Required = $true
    )

    if (Test-Path $Path) {
        if ($Detailed) {
            Write-Pass "$Name found at $Path"
        }
        return $true
    } else {
        if ($Required) {
            Write-Fail "$Name not found at $Path"
            $script:MissingDeps += $Name
        } else {
            Write-Warn "$Name not found at $Path (optional)"
        }
        return $false
    }
}

function Get-FileVersionString {
    param([string]$Path)

    if (-not (Test-Path $Path)) { return "N/A" }

    $item = Get-Item $Path
    if ($item.PSIsContainer) {
        return "(directory)"
    } else {
        return "$([math]::Round($item.Length / 1MB, 2)) MB"
    }
}

# === Header ===

Write-Info "=== AgentLoom Windows Dependency Validation ==="
Write-Info "Repository root: $RepoRoot"
Write-Info "Dependencies:    $DepsDir"
Write-Info "Media:           $Media"
Write-Info "Local LLM:       $LocalLlm"
Write-Info ""

if (-not (Test-Path $DepsDir)) {
    Write-Fail "Dependencies directory not found: $DepsDir"
    Write-Info "Run windows\scripts\prepare_deps.ps1 to download dependencies"
    exit 1
}

# === ONNX Runtime ===

Write-Info "[ONNX Runtime]"
$onnxCpuDir = Join-Path $DepsDir "onnxruntime-win-x64-1.17.1"
$onnxCpuDll = Join-Path $onnxCpuDir "lib\onnxruntime.dll"
$onnxCpuHeader = Join-Path $onnxCpuDir "include\onnxruntime_c_api.h"

$hasCpu = Test-DependencyPath -Path $onnxCpuDir -Name "ONNX Runtime CPU"
if ($hasCpu) {
    Test-DependencyPath -Path $onnxCpuDll -Name "ONNX Runtime DLL" | Out-Null
    Test-DependencyPath -Path $onnxCpuHeader -Name "ONNX Runtime headers" | Out-Null
    if ($Detailed) {
        Write-Info "    DLL size: $(Get-FileVersionString $onnxCpuDll)"
    }
}

$onnxGpuDir = Join-Path $DepsDir "onnxruntime-win-x64-gpu-1.20.1"
$onnxGpuDll = Join-Path $onnxGpuDir "lib\onnxruntime.dll"

if (Test-Path $onnxGpuDir) {
    Test-DependencyPath -Path $onnxGpuDll -Name "ONNX Runtime GPU DLL" -Required:$false | Out-Null
    if ($Detailed -and (Test-Path $onnxGpuDll)) {
        Write-Info "    GPU DLL size: $(Get-FileVersionString $onnxGpuDll)"
    }
}

Write-Info ""

# === SQLite ===

Write-Info "[SQLite]"
$sqliteDir = Join-Path $DepsDir "sqlite-amalgamation-3530100"
$sqliteHeader = Join-Path $sqliteDir "sqlite3.h"
$sqliteSource = Join-Path $sqliteDir "sqlite3.c"
$sqliteDll = Join-Path $DepsDir "sqlite3.dll"
$sqliteDef = Join-Path $DepsDir "sqlite3.def"

Test-DependencyPath -Path $sqliteDir -Name "SQLite amalgamation" | Out-Null
Test-DependencyPath -Path $sqliteHeader -Name "sqlite3.h" | Out-Null
Test-DependencyPath -Path $sqliteSource -Name "sqlite3.c" | Out-Null
Test-DependencyPath -Path $sqliteDll -Name "sqlite3.dll" | Out-Null
Test-DependencyPath -Path $sqliteDef -Name "sqlite3.def" | Out-Null

if ($Detailed -and (Test-Path $sqliteSource)) {
    $lines = (Get-Content -LiteralPath $sqliteSource -Encoding UTF8 | Measure-Object -Line).Lines
    Write-Info "    sqlite3.c: $lines lines"
}

Write-Info ""

# === Boost ===

Write-Info "[Boost]"
$boostDir = Join-Path $DepsDir "boost_1_85_0"
$boostVersion = Join-Path $boostDir "boost\version.hpp"
$boostAsio = Join-Path $boostDir "boost\asio.hpp"
$boostBeast = Join-Path $boostDir "boost\beast.hpp"
$boostRedis = Join-Path $boostDir "boost\redis.hpp"
$boostInterprocess = Join-Path $boostDir "boost\interprocess\managed_shared_memory.hpp"

Test-DependencyPath -Path $boostDir -Name "Boost headers" | Out-Null
Test-DependencyPath -Path $boostVersion -Name "boost/version.hpp" | Out-Null
Test-DependencyPath -Path $boostAsio -Name "boost/asio.hpp" | Out-Null
Test-DependencyPath -Path $boostBeast -Name "boost/beast.hpp" | Out-Null
Test-DependencyPath -Path $boostRedis -Name "boost/redis.hpp" | Out-Null
Test-DependencyPath -Path $boostInterprocess -Name "boost/interprocess (IPC)" | Out-Null

if ($Detailed -and (Test-Path $boostVersion)) {
    $versionLine = Get-Content -LiteralPath $boostVersion -Encoding UTF8 |
        Select-String '^#define BOOST_LIB_VERSION "[0-9_]+"$' | Select-Object -First 1
    Write-Info "    $($versionLine -replace '^\s+', '')"
}

Write-Info ""

# === Eigen ===

Write-Info "[Eigen]"
$eigenDir = Join-Path $DepsDir "eigen-5.0.1"
$eigenDense = Join-Path $eigenDir "Eigen\Dense"
$eigenCore = Join-Path $eigenDir "Eigen\Core"

Test-DependencyPath -Path $eigenDir -Name "Eigen headers" | Out-Null
Test-DependencyPath -Path $eigenDense -Name "Eigen/Dense" | Out-Null
Test-DependencyPath -Path $eigenCore -Name "Eigen/Core" | Out-Null

Write-Info ""

# === Faiss ===

Write-Info "[Faiss]"
$faissDir = Join-Path $DepsDir "faiss-1.14.1-cpu-win64"
$faissHeader = Join-Path $faissDir "include\faiss\IndexFlat.h"
$faissLib = Join-Path $faissDir "lib\faiss.lib"
$faissDll = Join-Path $faissDir "bin\faiss.dll"
$faissCmake = Join-Path $faissDir "share\faiss\faiss-config.cmake"

Test-DependencyPath -Path $faissDir -Name "Faiss package" | Out-Null
Test-DependencyPath -Path $faissHeader -Name "Faiss headers (IndexFlat.h)" | Out-Null
Test-DependencyPath -Path $faissLib -Name "faiss.lib" | Out-Null
Test-DependencyPath -Path $faissDll -Name "faiss.dll" | Out-Null
Test-DependencyPath -Path $faissCmake -Name "Faiss CMake config" | Out-Null

if ($Detailed) {
    if (Test-Path $faissLib) {
        Write-Info "    faiss.lib: $(Get-FileVersionString $faissLib)"
    }
    if (Test-Path $faissDll) {
        Write-Info "    faiss.dll: $(Get-FileVersionString $faissDll)"
    }
}

Write-Info ""

# === MKL Runtime ===

Write-Info "[MKL Runtime]"
$mklDir = Join-Path $DepsDir "mkl-2023.1.0-win64"
$mklDll = Join-Path $mklDir "Library\bin\mkl_rt.2.dll"

Test-DependencyPath -Path $mklDir -Name "MKL package" | Out-Null
Test-DependencyPath -Path $mklDll -Name "mkl_rt.2.dll" | Out-Null

if ($Detailed -and (Test-Path $mklDll)) {
    Write-Info "    mkl_rt.2.dll: $(Get-FileVersionString $mklDll)"
}

Write-Info ""

# === OpenCV ===

if ($Media) {
    Write-Info "[OpenCV]"
    $openCvDir = Join-Path $DepsDir "opencv-4.10.0-windows\build"
    $openCvCmake = Join-Path $openCvDir "x64\vc16\lib\OpenCVConfig.cmake"
    $openCvDll = Join-Path $openCvDir "x64\vc16\bin\opencv_world4100.dll"
    $openCvHeader = Join-Path $openCvDir "..\sources\modules\core\include\opencv2\core.hpp"

    Test-DependencyPath -Path $openCvDir -Name "OpenCV build" | Out-Null
    Test-DependencyPath -Path $openCvCmake -Name "OpenCV CMake config" | Out-Null
    Test-DependencyPath -Path $openCvDll -Name "OpenCV DLL (opencv_world)" | Out-Null
    Test-DependencyPath -Path $openCvHeader -Name "OpenCV headers (core.hpp)" | Out-Null

    if ($Detailed -and (Test-Path $openCvDll)) {
        Write-Info "    opencv_world DLL: $(Get-FileVersionString $openCvDll)"
    }
    Write-Info ""
}

# === HuggingFace Tokenizers (Rust source) ===

Write-Info "[HuggingFace Tokenizers]"
$hfTokenizerDir = Join-Path $RepoRoot "third_party\hf_tokenizers_capi"
$hfCargoToml = Join-Path $hfTokenizerDir "Cargo.toml"
$hfLibRs = Join-Path $hfTokenizerDir "src\lib.rs"
$hfHeader = Join-Path $hfTokenizerDir "include\hf_tokenizers_capi.h"

Test-DependencyPath -Path $hfTokenizerDir -Name "hf_tokenizers_capi" | Out-Null
Test-DependencyPath -Path $hfCargoToml -Name "Cargo.toml" | Out-Null
Test-DependencyPath -Path $hfLibRs -Name "src/lib.rs" | Out-Null
Test-DependencyPath -Path $hfHeader -Name "C API header" | Out-Null

if ($Detailed -and (Test-Path $hfCargoToml)) {
    $version = Get-Content -LiteralPath $hfCargoToml -Encoding UTF8 |
        Select-String 'tokenizers\s*=.*"([0-9.]+)"' |
        ForEach-Object { $_.Matches.Groups[1].Value }
    if ($version) {
        Write-Info "    tokenizers crate: $version"
    }
}

Write-Info ""

# === vcpkg packages ===

if (-not $SkipVcpkg) {
    Write-Info "[vcpkg packages]"
    $vcpkgInstalled = Join-Path $RepoRoot "vcpkg_installed"
    $tripletDir = Join-Path $vcpkgInstalled "x64-windows-release"
    $tripletDirFallback = Join-Path $vcpkgInstalled "x64-windows"

    $actualTriplet = if (Test-Path $tripletDir) { $tripletDir } elseif (Test-Path $tripletDirFallback) { $tripletDirFallback } else { $null }

    if (-not $actualTriplet) {
        Write-Fail "vcpkg_installed not found or empty"
        Write-Info "    Run windows\scripts\prepare_deps.ps1 to install vcpkg dependencies"
    } else {
        $tripletName = Split-Path -Leaf $actualTriplet
        Write-Pass "vcpkg triplet detected: $tripletName"

        # Check key packages
        $requiredPackages = @(
            @{ Name = "gRPC"; Path = "share\grpc\gRPCConfig.cmake" },
            @{ Name = "Protobuf"; Path = "share\protobuf\protobuf-config.cmake" },
            @{ Name = "OpenSSL"; Path = "share\openssl\OpenSSLConfig.cmake" },
            @{ Name = "spdlog"; Path = "share\spdlog\spdlogConfig.cmake" },
            @{ Name = "redis-plus-plus"; Path = "share\redis++\redis++-config.cmake" },
            @{ Name = "hiredis"; Path = "share\hiredis\hiredis-config.cmake" },
            @{ Name = "libzip"; Path = "share\libzip\libzip-config.cmake" },
            @{ Name = "pugixml"; Path = "share\pugixml\pugixml-config.cmake" }
        )

        foreach ($pkg in $requiredPackages) {
            $pkgPath = Join-Path $actualTriplet $pkg.Path
            Test-DependencyPath -Path $pkgPath -Name "  $($pkg.Name)" | Out-Null
        }

        if ($Detailed) {
            $infoDir = Join-Path $actualTriplet "..\vcpkg\info"
            if (Test-Path $infoDir) {
                $packageCount = (Get-ChildItem $infoDir -Filter "*.list" -ErrorAction SilentlyContinue | Measure-Object).Count
                Write-Info "    Total packages installed: $packageCount"
            }
        }
    }

    Write-Info ""
}

if ($LocalLlm) {
    Write-Info "[llama.cpp]"
    $llamaCppDir = $LlamaCppRoot
    $llamaBuildDir = $LlamaCppBuild
    $llamaLib = Join-Path $llamaBuildDir "src\Release\llama.lib"
    $llamaDll = Join-Path $llamaBuildDir "bin\Release\llama.dll"
    $mtmdLib = Join-Path $llamaBuildDir "tools\mtmd\Release\mtmd.lib"

    Test-DependencyPath -Path $llamaCppDir -Name "llama.cpp repository" | Out-Null
    Test-DependencyPath -Path $llamaBuildDir -Name "llama.cpp build directory" | Out-Null
    Test-DependencyPath -Path $llamaLib -Name "llama.lib" | Out-Null
    Test-DependencyPath -Path $llamaDll -Name "llama.dll" | Out-Null
    Test-DependencyPath -Path $mtmdLib -Name "mtmd.lib" | Out-Null
    Write-Info ""
}

# === Summary ===

Write-Info "=== Validation Summary ==="

if ($script:AllValid) {
    Write-Host "All required dependencies are present and valid" -ForegroundColor Green

    if ($script:MissingDeps.Count -eq 0) {
        Write-Info ""
        Write-Info "Ready to configure:"
        Write-Info '  cmake -B build\x64-Release-v145 -G "Visual Studio 18 2026" -A x64 \'
        Write-Info '    -DCMAKE_CONFIGURATION_TYPES=Release \'
        Write-Info '    -DBERT_VCPKG_TRIPLET=x64-windows-release \'
        Write-Info '    -DBERT_USE_ONNXRUNTIME_GPU=OFF \'
        Write-Info '    -DAGENTLOOM_BUILD_LOCAL_LLM=OFF \'
        Write-Info '    -DAGENTLOOM_BUILD_MEDIA=OFF'
    }

    exit 0
} else {
    Write-Host "Validation failed: $($script:MissingDeps.Count) missing dependencies" -ForegroundColor Red

    if ($script:MissingDeps.Count -gt 0) {
        Write-Info ""
        Write-Info "Missing dependencies:"
        foreach ($dep in $script:MissingDeps) {
            Write-Info "  - $dep"
        }
    }

    Write-Info ""
    Write-Info "Run the following to download all dependencies:"
    Write-Info "  windows\scripts\prepare_deps.ps1"

    exit 1
}
