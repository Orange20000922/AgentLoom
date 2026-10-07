#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="${APP_ROOT:-/app}"
APP_ROLE="${APP_ROLE:-gateway}"

resolve_default_config() {
    case "$1" in
        gateway)
            echo "$APP_ROOT/config/agent_gateway.container.example.json"
            ;;
        emotion-inference)
            echo "$APP_ROOT/config/emotion.json"
            ;;
        multimodal-inference)
            echo "$APP_ROOT/config/server.example.json"
            ;;
        *)
            return 1
            ;;
    esac
}

run_role() {
    local role="$1"
    shift || true

    local default_config
    default_config="$(resolve_default_config "$role")" || {
        echo "[docker-entrypoint] unknown APP_ROLE: $role" >&2
        exit 64
    }

    case "$role" in
        gateway)
            exec "$APP_ROOT/bin/agent_gateway_server" "${APP_CONFIG:-$default_config}" "$@"
            ;;
        emotion-inference)
            exec "$APP_ROOT/bin/emotion_inference_server" --config "${APP_CONFIG:-$default_config}" "$@"
            ;;
        multimodal-inference)
            exec "$APP_ROOT/bin/multimodal_inference_server" --config "${APP_CONFIG:-$default_config}" "$@"
            ;;
    esac
}

if (($# > 0)); then
    case "$1" in
        gateway|emotion-inference|multimodal-inference)
            role="$1"
            shift
            run_role "$role" "$@"
            ;;
        *)
            exec "$@"
            ;;
    esac
else
    run_role "$APP_ROLE"
fi
