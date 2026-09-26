# Download and verify ONNX Runtime for Windows
# Usage: .\download-onnxruntime.ps1 <url> <archive-path> <deps-dir> <extract-root>

param(
    [Parameter(Mandatory=$true)]
    [string]$Url,

    [Parameter(Mandatory=$true)]
    [string]$ArchivePath,

    [Parameter(Mandatory=$true)]
    [string]$DepsDir,

    [Parameter(Mandatory=$true)]
    [string]$ExtractRoot
)

$ErrorActionPreference = "Stop"

function Download-File {
    param([string]$Url, [string]$Path)

    if (Test-Path $Path) {
        Write-Host "Archive already exists: $Path"
        return
    }

    Write-Host "Downloading ONNX Runtime from: $Url"
    $tempPath = "$Path.tmp"
    Remove-Item -Path $tempPath -ErrorAction SilentlyContinue

    Invoke-WebRequest -Uri $Url -OutFile $tempPath -MaximumRetryCount 5 -RetryIntervalSec 5

    if (-not (Test-Path $tempPath)) {
        throw "Download failed: $Url"
    }

    Move-Item -Path $tempPath -Destination $Path -Force
    Write-Host "Downloaded to: $Path"
}

function Verify-Zip {
    param([string]$Path)

    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
        $zip.Dispose()
        return $true
    } catch {
        return $false
    }
}

# Create deps directory
New-Item -ItemType Directory -Path $DepsDir -Force | Out-Null

# Download archive if needed
if (-not (Test-Path $ArchivePath)) {
    Download-File -Url $Url -Path $ArchivePath
}

# Verify archive integrity
if (-not (Verify-Zip -Path $ArchivePath)) {
    Write-Host "Archive is corrupted, re-downloading..."
    Remove-Item -Path $ArchivePath -Force
    Download-File -Url $Url -Path $ArchivePath

    if (-not (Verify-Zip -Path $ArchivePath)) {
        throw "Archive is still corrupted after re-download: $ArchivePath"
    }
}

# Extract if not already extracted
if (-not (Test-Path $ExtractRoot)) {
    Write-Host "Extracting ONNX Runtime to: $ExtractRoot"
    Expand-Archive -Path $ArchivePath -DestinationPath $DepsDir -Force
}

# Verify extraction
$requiredFiles = @(
    "$ExtractRoot\include\onnxruntime\onnxruntime_c_api.h",
    "$ExtractRoot\lib\onnxruntime.lib"
)

foreach ($file in $requiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found after extraction: $file"
    }
}

Write-Host "ONNX Runtime is ready at: $ExtractRoot"
