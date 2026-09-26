#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_linux
require_command file
require_command ldd

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

if [[ "${1:-}" == "--inference" ]]; then
    artifacts+=(multimodal_inference_server)
fi

for artifact in "${artifacts[@]}"; do
    path="$BUILD_DIR/$artifact"
    [[ -x "$path" ]] || fail "missing executable: $path"
    file "$path"
    file "$path" | grep -q "ELF 64-bit" || fail "not a Linux x64 ELF binary: $path"
    if ldd "$path" | grep -q "not found"; then
        ldd "$path"
        fail "unresolved runtime library for $path"
    fi
done

log "all requested Linux artifacts are valid ELF binaries"
