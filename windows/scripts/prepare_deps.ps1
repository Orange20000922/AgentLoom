# Prepare AgentLoom dependencies for Windows.
# Core/SDK dependencies are prepared by default. Use -Media to add OpenCV;
# llama.cpp and GStreamer remain explicit external SDK inputs.

[CmdletBinding()]
param(
    [switch]$SkipVcpkg,
    [switch]$Media,
    [switch]$Force
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = $PSScriptRoot
$WindowsDir = Split-Path -Parent $ScriptDir
$RepoRoot = Split-Path -Parent $WindowsDir
$DepsDir = Join-Path $RepoRoot "deps"
$BuildDir = Join-Path $RepoRoot "build"
$VcpkgRoot = Join-Path $RepoRoot "vcpkg"
$VcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
$VcpkgTriplet = "x64-windows-release"
$PythonVenv = Join-Path $BuildDir "windows-python-venv"
$PythonExe = Join-Path $PythonVenv "Scripts\python.exe"
$CondaExtractor = Join-Path $RepoRoot ".github\scripts\extract-conda-package.py"

$OnnxRuntimeVersion = "1.17.1"
$SqliteVersion = "3530100"
$BoostVersion = "1_85_0"
$EigenVersion = "5.0.1"
$FaissVersion = "1.14.1"
$MklVersion = "2023.1.0"
$OpenCvVersion = "4.10.0"

$OnnxRuntimeUrl = "https://github.com/microsoft/onnxruntime/releases/download/v$OnnxRuntimeVersion/onnxruntime-win-x64-$OnnxRuntimeVersion.zip"
$SqliteUrl = "https://www.sqlite.org/2026/sqlite-amalgamation-$SqliteVersion.zip"
$SqliteDllUrl = "https://www.sqlite.org/2026/sqlite-dll-win-x64-$SqliteVersion.zip"
$BoostUrl = "https://archives.boost.io/release/1.85.0/source/boost_$BoostVersion.zip"
$EigenUrl = "https://gitlab.com/libeigen/eigen/-/archive/$EigenVersion/eigen-$EigenVersion.zip"
$DefaultFaissUrl = "https://conda.anaconda.org/pytorch/win-64/libfaiss-1.14.1-py3.12_h2e52968_0_cpu.conda"
$DefaultFaissSha256 = "D42FF92B45F8D202C8FA205815690D876C26AF22295546E31F8253EB77C369FA"
$DefaultMklUrl = "https://repo.anaconda.com/pkgs/main/win-64/mkl-2023.1.0-h6b88ed4_46358.conda"
$DefaultMklSha256 = "2B06DA1AE1ED81D9A798395604C7653365E2B197A078FDE6FD47DC6218B730BB"
$OpenCvUrl = "https://github.com/opencv/opencv/releases/download/$OpenCvVersion/opencv-$OpenCvVersion-windows.exe"
$OpenCvSha256 = "BFF38466091C313DAC21A0B73EEA8278316A89C1D434C6F0B10697E087670168"

$FaissUrl = if ($env:FAISS_URL) { $env:FAISS_URL } else { $DefaultFaissUrl }
$FaissSha256 = if ($env:FAISS_SHA256) {
    $env:FAISS_SHA256
} elseif ($env:FAISS_URL) {
    ""
} else {
    $DefaultFaissSha256
}
$MklUrl = if ($env:MKL_URL) { $env:MKL_URL } else { $DefaultMklUrl }
$MklSha256 = if ($env:MKL_SHA256) {
    $env:MKL_SHA256
} elseif ($env:MKL_URL) {
    ""
} else {
    $DefaultMklSha256
}

$OnnxRuntimeRoot = Join-Path $DepsDir "onnxruntime-win-x64-$OnnxRuntimeVersion"
$SqliteRoot = Join-Path $DepsDir "sqlite-amalgamation-$SqliteVersion"
$BoostRoot = Join-Path $DepsDir "boost_$BoostVersion"
$EigenRoot = Join-Path $DepsDir "eigen-$EigenVersion"
$FaissRoot = Join-Path $DepsDir "faiss-$FaissVersion-cpu-win64"
$MklRoot = Join-Path $DepsDir "mkl-$MklVersion-win64"
$OpenCvRoot = Join-Path $DepsDir "opencv-$OpenCvVersion-windows"

function Write-Log {
    param([string]$Message)
    Write-Host "[prepare-deps] $Message" -ForegroundColor Cyan
}

function Test-RequiredPaths {
    param([string[]]$Paths)
    foreach ($path in $Paths) {
        if (-not (Test-Path -LiteralPath $path)) {
            return $false
        }
    }
    return $true
}

function Test-FileHash {
    param(
        [string]$Path,
        [string]$ExpectedSha256
    )
    if (-not $ExpectedSha256) {
        return $true
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    return $actual.Equals($ExpectedSha256, [StringComparison]::OrdinalIgnoreCase)
}

function Download-File {
    param(
        [string]$Url,
        [string]$OutputPath,
        [string]$Sha256 = ""
    )

    if ((Test-Path -LiteralPath $OutputPath) -and -not $Force) {
        if (Test-FileHash -Path $OutputPath -ExpectedSha256 $Sha256) {
            Write-Log "Using cached archive: $OutputPath"
            return
        }
        Write-Log "Removing archive with unexpected SHA-256: $OutputPath"
        Remove-Item -LiteralPath $OutputPath -Force
    }

    $parent = Split-Path -Parent $OutputPath
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $tempPath = "$OutputPath.tmp"
    Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue

    Write-Log "Downloading $Url"
    $lastError = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            $client = New-Object System.Net.WebClient
            try {
                $client.DownloadFile($Url, $tempPath)
            }
            finally {
                $client.Dispose()
            }
            $lastError = $null
            break
        }
        catch {
            $lastError = $_
            Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
            if ($attempt -lt 3) {
                Start-Sleep -Seconds (5 * $attempt)
            }
        }
    }
    if ($lastError) {
        throw "Failed to download $Url after three attempts: $lastError"
    }

    if (-not (Test-FileHash -Path $tempPath -ExpectedSha256 $Sha256)) {
        Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
        throw "SHA-256 verification failed for $Url"
    }
    Move-Item -LiteralPath $tempPath -Destination $OutputPath -Force
}

function Expand-ZipDependency {
    param(
        [string]$ArchivePath,
        [string]$DestinationRoot,
        [string]$ExistingRoot
    )
    if ($ExistingRoot -and (Test-Path -LiteralPath $ExistingRoot)) {
        Remove-Item -LiteralPath $ExistingRoot -Recurse -Force
    }
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $DestinationRoot -Force
}

function Ensure-PythonEnvironment {
    if (-not (Test-Path -LiteralPath $PythonExe)) {
        $systemPython = Get-Command python -ErrorAction SilentlyContinue
        if (-not $systemPython) {
            throw "Python 3.12 or newer is required to extract .conda packages"
        }
        Write-Log "Creating private Python environment: $PythonVenv"
        & $systemPython.Source -m venv $PythonVenv
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to create Python environment at $PythonVenv"
        }
    }

    & $PythonExe -c "import zstandard" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Log "Installing zstandard into the private Python environment"
        & $PythonExe -m pip install --disable-pip-version-check "zstandard>=0.22,<1"
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install Python zstandard package"
        }
    }
}

function Expand-CondaDependency {
    param(
        [string]$ArchivePath,
        [string]$DestinationPath
    )
    Ensure-PythonEnvironment
    & $PythonExe $CondaExtractor $ArchivePath $DestinationPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to extract Conda package: $ArchivePath"
    }
}

function Ensure-Vcpkg {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw "git is required to bootstrap vcpkg"
    }

    $manifest = Join-Path $RepoRoot "vcpkg.json"
    $baseline = (Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 |
        ConvertFrom-Json).'builtin-baseline'
    if (-not $baseline) {
        throw "vcpkg.json does not define builtin-baseline"
    }

    if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot ".git"))) {
        if (Test-Path -LiteralPath $VcpkgRoot) {
            $entries = @(Get-ChildItem -LiteralPath $VcpkgRoot -Force)
            if ($entries.Count -gt 0) {
                throw "vcpkg directory exists but is not a git checkout: $VcpkgRoot"
            }
        } else {
            New-Item -ItemType Directory -Path $VcpkgRoot -Force | Out-Null
        }
        & git init $VcpkgRoot | Out-Null
        & git -C $VcpkgRoot remote add origin https://github.com/microsoft/vcpkg.git
    }

    $needsBootstrap = -not (Test-Path -LiteralPath $VcpkgExe)
    $current = (& git -C $VcpkgRoot rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or $current -ne $baseline) {
        Write-Log "Fetching vcpkg baseline $baseline"
        & git -C $VcpkgRoot fetch --no-tags origin $baseline
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to fetch vcpkg baseline $baseline"
        }
        & git -C $VcpkgRoot checkout --detach FETCH_HEAD
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to checkout vcpkg baseline $baseline"
        }
        $needsBootstrap = $true
    }

    if ($needsBootstrap) {
        Write-Log "Bootstrapping vcpkg"
        & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $VcpkgExe)) {
            throw "vcpkg bootstrap failed: $VcpkgExe"
        }
    }
    return $VcpkgExe
}

Write-Log "=== AgentLoom Windows dependency preparation ==="
Write-Log "Repository: $RepoRoot"
Write-Log "Profile:    $(if ($Media) { 'Core/SDK + Media' } else { 'Core/SDK' })"
New-Item -ItemType Directory -Path $DepsDir -Force | Out-Null

$onnxRequired = @(
    (Join-Path $OnnxRuntimeRoot "include\onnxruntime_c_api.h"),
    (Join-Path $OnnxRuntimeRoot "lib\onnxruntime.dll"),
    (Join-Path $OnnxRuntimeRoot "lib\onnxruntime.lib")
)
if ($Force -or -not (Test-RequiredPaths $onnxRequired)) {
    $archive = Join-Path $DepsDir "onnxruntime-win-x64-$OnnxRuntimeVersion.zip"
    Download-File $OnnxRuntimeUrl $archive
    Expand-ZipDependency $archive $DepsDir $OnnxRuntimeRoot
}

$sqliteRequired = @(
    (Join-Path $SqliteRoot "sqlite3.h"),
    (Join-Path $SqliteRoot "sqlite3.c")
)
if ($Force -or -not (Test-RequiredPaths $sqliteRequired)) {
    $archive = Join-Path $DepsDir "sqlite-amalgamation-$SqliteVersion.zip"
    Download-File $SqliteUrl $archive
    Expand-ZipDependency $archive $DepsDir $SqliteRoot
}

$sqliteRuntimeRequired = @(
    (Join-Path $DepsDir "sqlite3.dll"),
    (Join-Path $DepsDir "sqlite3.def")
)
if ($Force -or -not (Test-RequiredPaths $sqliteRuntimeRequired)) {
    $archive = Join-Path $DepsDir "sqlite-dll-win-x64-$SqliteVersion.zip"
    Download-File $SqliteDllUrl $archive
    if ($Force) {
        Remove-Item -LiteralPath (Join-Path $DepsDir "sqlite3.dll") -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath (Join-Path $DepsDir "sqlite3.def") -Force -ErrorAction SilentlyContinue
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $DepsDir -Force
}

$boostRequired = @(
    (Join-Path $BoostRoot "boost\version.hpp"),
    (Join-Path $BoostRoot "boost\redis.hpp")
)
if ($Force -or -not (Test-RequiredPaths $boostRequired)) {
    $archive = Join-Path $DepsDir "boost_$BoostVersion.zip"
    Download-File $BoostUrl $archive
    Expand-ZipDependency $archive $DepsDir $BoostRoot
}

$eigenRequired = @((Join-Path $EigenRoot "Eigen\Core"))
if ($Force -or -not (Test-RequiredPaths $eigenRequired)) {
    $archive = Join-Path $DepsDir "eigen-$EigenVersion.zip"
    Download-File $EigenUrl $archive
    Expand-ZipDependency $archive $DepsDir $EigenRoot
}

$faissRequired = @(
    (Join-Path $FaissRoot "include\faiss\IndexFlat.h"),
    (Join-Path $FaissRoot "lib\faiss.lib"),
    (Join-Path $FaissRoot "bin\faiss.dll"),
    (Join-Path $FaissRoot "share\faiss\faiss-config.cmake")
)
if ($Force -or -not (Test-RequiredPaths $faissRequired)) {
    $archive = Join-Path $DepsDir "libfaiss-1.14.1-py3.12_h2e52968_0_cpu.conda"
    Download-File $FaissUrl $archive $FaissSha256
    Expand-CondaDependency $archive $FaissRoot
}

$mklRequired = @((Join-Path $MklRoot "Library\bin\mkl_rt.2.dll"))
if ($Force -or -not (Test-RequiredPaths $mklRequired)) {
    $archive = Join-Path $DepsDir "mkl-2023.1.0-h6b88ed4_46358.conda"
    Download-File $MklUrl $archive $MklSha256
    Expand-CondaDependency $archive $MklRoot
}

if ($Media) {
    $openCvRequired = @(
        (Join-Path $OpenCvRoot "build\x64\vc16\lib\OpenCVConfig.cmake"),
        (Join-Path $OpenCvRoot "build\x64\vc16\bin\opencv_world4100.dll")
    )
    if ($Force -or -not (Test-RequiredPaths $openCvRequired)) {
        $archive = Join-Path $DepsDir "opencv-$OpenCvVersion-windows.exe"
        Download-File $OpenCvUrl $archive $OpenCvSha256
        $extractRoot = Join-Path $DepsDir ".opencv-$OpenCvVersion-extract"
        Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $OpenCvRoot -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
        & $archive "-o$extractRoot" -y | Out-Null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $extractRoot "opencv"))) {
            throw "Failed to extract OpenCV archive: $archive"
        }
        Move-Item -LiteralPath (Join-Path $extractRoot "opencv") -Destination $OpenCvRoot
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
}

$coreRequired = $onnxRequired + $sqliteRequired + $sqliteRuntimeRequired +
    $boostRequired + $eigenRequired + $faissRequired + $mklRequired
if ($Media) {
    $coreRequired += $openCvRequired
}
if (-not (Test-RequiredPaths $coreRequired)) {
    throw "One or more required Core/SDK dependencies are incomplete"
}

if (-not $SkipVcpkg) {
    $tripletRoot = Join-Path $RepoRoot "triplets\ci"
    $tripletFile = Join-Path $tripletRoot "$VcpkgTriplet.cmake"
    if (-not (Test-Path -LiteralPath $tripletFile)) {
        throw "Versioned vcpkg triplet not found: $tripletFile"
    }

    $vcpkg = Ensure-Vcpkg
    $installRoot = Join-Path $RepoRoot "vcpkg_installed"
    $binaryCache = Join-Path $BuildDir "vcpkg-binary-cache-windows"
    New-Item -ItemType Directory -Path $binaryCache -Force | Out-Null

    & $vcpkg install `
        --triplet $VcpkgTriplet `
        --host-triplet $VcpkgTriplet `
        --x-feature=tests `
        --x-install-root="$installRoot" `
        --x-manifest-root="$RepoRoot" `
        --overlay-triplets="$tripletRoot" `
        "--binarysource=clear;files,$binaryCache,readwrite" `
        --clean-after-build
    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg install failed with exit code $LASTEXITCODE"
    }
}

Write-Log "Dependencies prepared successfully"
Write-Log "ONNX Runtime: $OnnxRuntimeRoot"
Write-Log "SQLite:       $SqliteRoot"
Write-Log "Boost:        $BoostRoot"
Write-Log "Eigen:        $EigenRoot"
Write-Log "Faiss:        $FaissRoot"
Write-Log "MKL runtime:  $MklRoot"
if ($Media) {
    Write-Log "OpenCV:       $OpenCvRoot"
}
Write-Log "Next: .\windows\scripts\configure.ps1 -Tests"
