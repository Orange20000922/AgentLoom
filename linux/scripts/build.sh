#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_linux
require_command cmake
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || fail "build is not configured; run linux/scripts/configure.sh first"

targets=(
    agentloom_sdk
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
    bert_inference_client
    bert_benchmark_client
)

if cmake --build "$BUILD_DIR" --target help | grep -q 'gateway_service_tests'; then
    targets+=(gateway_service_tests)
fi
if cmake --build "$BUILD_DIR" --target help | grep -q 'media_skill_tests'; then
    targets+=(media_skill_tests)
fi

if [[ "${1:-}" == "--inference" ]]; then
    has_cuda_llama_binary || fail "--inference requires a prepared CUDA llama.cpp binary"
    targets+=(multimodal_inference_server)
fi

cmake --build "$BUILD_DIR" --target "${targets[@]}" --parallel "$BUILD_JOBS"
log "built targets: ${targets[*]}"
