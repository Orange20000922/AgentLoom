#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_linux
require_command cmake
require_command file
require_command ldd

PACKAGE_DIR="${PACKAGE_DIR:-$REPO_ROOT/build/linux-package}"

artifacts=(
    core_tests
    net_tests
    http_client_tests
    tls_tests
    storage_tests
    vector_storage_tests
    semantic_cache_tests
    document_tests
    memory_tests
    vector_tests
    config_tests
    persona_runtime_tests
    llm_tests
    llm_integration_tests
    l3_compression_e2e_test
    document_analysis_e2e_test
    persona_gateway_e2e_server
    agent_gateway_server
    emotion_inference_server
)

if [[ -x "$BUILD_DIR/gateway_service_tests" ]]; then
    artifacts+=(gateway_service_tests)
fi
if [[ -x "$BUILD_DIR/media_skill_tests" ]]; then
    artifacts+=(media_skill_tests)
fi

include_inference=false
for arg in "$@"; do
    case "$arg" in
        --inference)
            include_inference=true
            ;;
        *)
            fail "unknown package option: $arg"
            ;;
    esac
done

if [[ "$include_inference" == true ]]; then
    has_cuda_llama_binary || fail "--inference requires a prepared CUDA llama.cpp binary"
    artifacts+=(multimodal_inference_server)
fi

[[ -d "$BUILD_DIR" ]] || fail "build directory not found: $BUILD_DIR"

rm -rf "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR/bin" "$PACKAGE_DIR/config" "$PACKAGE_DIR/tools" "$PACKAGE_DIR/dist"

cmake --install "$BUILD_DIR" --prefix "$PACKAGE_DIR/sdk"

declare -a copied_elfs=()
for artifact in "${artifacts[@]}"; do
    src="$BUILD_DIR/$artifact"
    [[ -x "$src" ]] || fail "missing executable: $src"
    file "$src" | grep -q "ELF 64-bit" || fail "not a Linux x64 ELF binary: $src"
    cp -L "$src" "$PACKAGE_DIR/bin/$artifact"
    copied_elfs+=("$PACKAGE_DIR/bin/$artifact")
done

copy_if_present() {
    local src="$1"
    local dst="$2"
    if [[ -e "$src" ]]; then
        mkdir -p "$(dirname "$dst")"
        cp -a "$src" "$dst"
    fi
}

copy_if_present "$REPO_ROOT/config.example.json" "$PACKAGE_DIR/config/config.example.json"
copy_if_present "$REPO_ROOT/tools/persona_gateway_e2e_server.json" "$PACKAGE_DIR/config/persona_gateway_e2e_server.json"
copy_if_present "$REPO_ROOT/tools/llm_smoke_test.example.json" "$PACKAGE_DIR/config/llm_smoke_test.example.json"
copy_if_present "$REPO_ROOT/tools/l3_compression_e2e_test.json" "$PACKAGE_DIR/config/l3_compression_e2e_test.json"
copy_if_present "$REPO_ROOT/tools/llm_smoke_prompt.txt" "$PACKAGE_DIR/tools/llm_smoke_prompt.txt"

if [[ -d "$REPO_ROOT/dist" ]]; then
    cp -a "$REPO_ROOT/dist/." "$PACKAGE_DIR/dist/"
fi

is_packaged_runtime_lib() {
    local path="$1"
    local base
    base="$(basename "$path")"
    case "$base" in
        ld-linux-*|linux-vdso.so.*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

collect_ldd_paths() {
    local elf="$1"
    ldd "$elf" | awk '
        /=>/ && $3 ~ /^\// { print $3 }
        /^[[:space:]]*\// { print $1 }
    '
}

declare -A seen_libs=()
queue=("${copied_elfs[@]}")

while ((${#queue[@]} > 0)); do
    current="${queue[0]}"
    queue=("${queue[@]:1}")

    while IFS= read -r lib; do
        [[ -n "$lib" ]] || continue
        [[ -e "$lib" ]] || continue
        is_packaged_runtime_lib "$lib" || continue

        real_lib="$(readlink -f "$lib")"
        [[ -n "${seen_libs[$real_lib]:-}" ]] && continue
        seen_libs["$real_lib"]=1

        dst="$PACKAGE_DIR/bin/$(basename "$lib")"
        if [[ "$(readlink -f "$dst" 2>/dev/null || true)" == "$real_lib" ]]; then
            continue
        fi
        cp -L "$lib" "$dst"
        chmod u+w "$dst" 2>/dev/null || true

        if file "$dst" | grep -q "ELF"; then
            queue+=("$dst")
        fi
    done < <(collect_ldd_paths "$current")
done

for elf in "${copied_elfs[@]}"; do
    if LD_LIBRARY_PATH="$PACKAGE_DIR/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$elf" | grep -q "not found"; then
        LD_LIBRARY_PATH="$PACKAGE_DIR/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$elf"
        fail "unresolved runtime library in package for $(basename "$elf")"
    fi
done

cat > "$PACKAGE_DIR/README.txt" <<EOF
AgentLoom Linux package

Run from this directory or set LD_LIBRARY_PATH to bin:

  cd "$PACKAGE_DIR"
  LD_LIBRARY_PATH=bin ./bin/agent_gateway_server config/persona_gateway_e2e_server.json

Useful binaries:
  bin/agent_gateway_server
  bin/persona_gateway_e2e_server
  bin/emotion_inference_server
  bin/document_analysis_e2e_test
  bin/l3_compression_e2e_test

Configuration templates are under config/.
Frontend static files, if present at package time, are under dist/.
The reusable CMake SDK is under sdk/ and can be consumed with:

  find_package(AgentLoom CONFIG REQUIRED)
  target_link_libraries(my_agent PRIVATE AgentLoom::core AgentLoom::runtime AgentLoom::gateway)
EOF

{
    echo "package_dir=$PACKAGE_DIR"
    echo "build_dir=$BUILD_DIR"
    echo "created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    echo "[artifacts]"
    printf '%s\n' "${artifacts[@]}"
    echo
    echo "[runtime_libraries]"
    find "$PACKAGE_DIR/bin" -maxdepth 1 -type f -name '*.so*' -printf '%f\n' | sort
    echo
    echo "[sdk]"
    echo "prefix=sdk"
    echo "config=sdk/lib/cmake/AgentLoom/AgentLoomConfig.cmake"
} > "$PACKAGE_DIR/manifest.txt"

log "Linux package created at $PACKAGE_DIR"
