#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"

required_files=(
    "vcpkg.json"
    "rust-toolchain.toml"
    ".github/scripts/ensure-disk-space.sh"
    "triplets/ci/x64-linux-release.cmake"
    "third_party/hf_tokenizers_capi/Cargo.toml"
    "third_party/hf_tokenizers_capi/Cargo.lock"
    "third_party/hf_tokenizers_capi/include/hf_tokenizers_capi.h"
    "third_party/hf_tokenizers_capi/src/lib.rs"
)

missing=()
for relative_path in "${required_files[@]}"; do
    if [[ ! -f "$repo_root/$relative_path" ]]; then
        missing+=("$relative_path")
    fi
done

if (( ${#missing[@]} > 0 )); then
    printf 'Required build inputs are missing from this checkout:\n' >&2
    printf '  - %s\n' "${missing[@]}" >&2
    exit 1
fi

if ! grep -Eq '^set\(VCPKG_BUILD_TYPE[[:space:]]+release\)$' \
        "$repo_root/triplets/ci/x64-linux-release.cmake"; then
    printf 'triplets/ci/x64-linux-release.cmake must enforce a release-only vcpkg build\n' >&2
    exit 1
fi

printf 'Build inputs are complete and version controlled.\n'
