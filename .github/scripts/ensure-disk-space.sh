#!/usr/bin/env bash
set -euo pipefail

target_path="${1:-${GITHUB_WORKSPACE:-.}}"
minimum_gib="${MIN_FREE_DISK_GIB:-35}"

if [[ ! "$minimum_gib" =~ ^[0-9]+$ ]] || (( minimum_gib <= 0 )); then
    printf 'MIN_FREE_DISK_GIB must be a positive integer, got: %s\n' "$minimum_gib" >&2
    exit 1
fi

available_kib="$(df -Pk "$target_path" | awk 'NR == 2 {print $4}')"
minimum_kib="$((minimum_gib * 1024 * 1024))"

printf 'Disk space before cleanup:\n'
df -h "$target_path"

if (( available_kib >= minimum_kib )); then
    printf 'Skipping runner cleanup: %s GiB free meets the %s GiB threshold.\n' \
        "$((available_kib / 1024 / 1024))" "$minimum_gib"
    exit 0
fi

printf 'Runner has only %s GiB free; reclaiming preinstalled SDK and image space.\n' \
    "$((available_kib / 1024 / 1024))"
sudo rm -rf \
    /usr/share/dotnet \
    /usr/local/lib/android \
    /opt/ghc \
    /usr/local/.ghcup \
    /usr/share/swift
sudo docker image prune -af || true
sudo apt-get clean

available_kib="$(df -Pk "$target_path" | awk 'NR == 2 {print $4}')"
printf 'Disk space after cleanup:\n'
df -h "$target_path"

if (( available_kib < minimum_kib )); then
    printf 'Runner still has only %s GiB free; at least %s GiB is required.\n' \
        "$((available_kib / 1024 / 1024))" "$minimum_gib" >&2
    exit 1
fi
