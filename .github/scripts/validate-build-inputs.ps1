# Validate version-controlled Windows build inputs before downloading dependencies.

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ScriptDir = $PSScriptRoot
$RepoRoot = Split-Path -Parent (Split-Path -Parent $ScriptDir)

$requiredFiles = @(
    "vcpkg.json",
    "rust-toolchain.toml",
    ".github\scripts\ensure-disk-space.ps1",
    ".github\scripts\extract-conda-package.py",
    "triplets\ci\x64-windows-release.cmake",
    "third_party\hf_tokenizers_capi\Cargo.toml",
    "third_party\hf_tokenizers_capi\Cargo.lock",
    "third_party\hf_tokenizers_capi\include\hf_tokenizers_capi.h",
    "third_party\hf_tokenizers_capi\src\lib.rs"
)

$missing = @(
    $requiredFiles | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $RepoRoot $_) -PathType Leaf)
    }
)
if ($missing.Count -gt 0) {
    $missing | ForEach-Object { Write-Error "Missing versioned build input: $_" }
    exit 1
}

$manifestPath = Join-Path $RepoRoot "vcpkg.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $manifest.'builtin-baseline') {
    throw "vcpkg.json does not define builtin-baseline"
}

$tripletPath = Join-Path $RepoRoot "triplets\ci\x64-windows-release.cmake"
$triplet = Get-Content -LiteralPath $tripletPath -Raw -Encoding UTF8
if ($triplet -notmatch 'set\(VCPKG_BUILD_TYPE\s+release\)') {
    throw "x64-windows-release.cmake must enforce a release-only vcpkg build"
}

$extractorPath = Join-Path $RepoRoot ".github\scripts\extract-conda-package.py"
$extractor = Get-Content -LiteralPath $extractorPath -Raw -Encoding UTF8
if ($extractor -notmatch 'filter="data"') {
    throw "extract-conda-package.py must use safe tar extraction"
}

Write-Host "Build inputs are complete and version controlled."
Write-Host "vcpkg baseline: $($manifest.'builtin-baseline')"
