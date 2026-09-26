#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_linux
for command in cmake ninja; do
    require_command "$command"
done
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || fail "build is not configured; run linux/scripts/configure.sh first"

rm -rf "$LINUX_INSTALL_DIR" "$PACKAGE_CONSUMER_BUILD_DIR"
cmake --install "$BUILD_DIR" --prefix "$LINUX_INSTALL_DIR"

cmake -S "$REPO_ROOT/tests/package/consumer" -B "$PACKAGE_CONSUMER_BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBOOST_ROOT="$BOOST_ROOT" \
    "-DCMAKE_PREFIX_PATH=$LINUX_INSTALL_DIR;$VCPKG_INSTALL_ROOT"
cmake --build "$PACKAGE_CONSUMER_BUILD_DIR" --parallel "$BUILD_JOBS"
"$PACKAGE_CONSUMER_BUILD_DIR/agentloom_package_consumer"

log "verified Linux install-tree find_package consumer at $PACKAGE_CONSUMER_BUILD_DIR"
