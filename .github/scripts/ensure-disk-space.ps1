# Ensure sufficient disk space for Windows CI builds
# Removes unnecessary files to free up space on GitHub Actions runners

param(
    [int]$MinFreeGiB = 30
)

$ErrorActionPreference = "Stop"

function Get-DiskFreeSpaceGB {
    $drive = Get-PSDrive -Name C
    return [math]::Round($drive.Free / 1GB, 2)
}

function Remove-DirectoryIfExists {
    param([string]$Path)

    if (Test-Path $Path) {
        $sizeBefore = (Get-ChildItem $Path -Recurse -ErrorAction SilentlyContinue |
                      Measure-Object -Property Length -Sum).Sum / 1GB
        Write-Host "Removing: $Path (approximately $([math]::Round($sizeBefore, 2)) GiB)"
        Remove-Item -Path $Path -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "=== Disk Space Check ==="
$freeSpace = Get-DiskFreeSpaceGB
Write-Host "Current free space: $freeSpace GiB"
Write-Host "Required free space: $MinFreeGiB GiB"

if ($freeSpace -ge $MinFreeGiB) {
    Write-Host "Sufficient disk space available"
    exit 0
}

Write-Host ""
Write-Host "=== Cleaning up to free disk space ==="

# Remove Android SDK (often 10+ GiB)
Remove-DirectoryIfExists "C:\Android"

# Remove .NET runtimes we don't need
Remove-DirectoryIfExists "C:\Program Files\dotnet\shared\Microsoft.AspNetCore.App"
Remove-DirectoryIfExists "C:\Program Files\dotnet\shared\Microsoft.WindowsDesktop.App"

# Remove other large tools
Remove-DirectoryIfExists "C:\hostedtoolcache\CodeQL"
Remove-DirectoryIfExists "C:\hostedtoolcache\go"
Remove-DirectoryIfExists "C:\hostedtoolcache\PyPy"

# Remove large caches
Remove-DirectoryIfExists "$env:LOCALAPPDATA\Temp"
Remove-DirectoryIfExists "$env:TEMP"

Write-Host ""
Write-Host "=== Cleanup Complete ==="
$freeSpaceAfter = Get-DiskFreeSpaceGB
$freed = $freeSpaceAfter - $freeSpace
Write-Host "Free space after cleanup: $freeSpaceAfter GiB"
Write-Host "Space freed: $([math]::Round($freed, 2)) GiB"

if ($freeSpaceAfter -lt $MinFreeGiB) {
    Write-Warning "Still insufficient disk space after cleanup"
    exit 1
}

Write-Host "Disk space requirements met"
