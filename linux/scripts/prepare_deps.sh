#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/common.sh"

require_linux
for command in curl git python3 tar unzip; do
    require_command "$command"
done
[[ -x "$PYTHON_BIN" ]] || fail "Python venv not found at $PYTHON_BIN; run linux/scripts/bootstrap_toolchain.sh first"
bash "$REPO_ROOT/.github/scripts/validate-build-inputs.sh"

mkdir -p "$DEPS_DIR" "$REPO_ROOT/triplets/ci" "$VCPKG_BINARY_CACHE"
log "preparing Linux dependencies under $DEPS_DIR"

repo_vcpkg_installed="$(cd "$REPO_ROOT" && pwd -P)/vcpkg_installed"
target_vcpkg_installed="$(mkdir -p "$VCPKG_INSTALL_ROOT" && cd "$VCPKG_INSTALL_ROOT" && pwd -P)"
if [[ "$target_vcpkg_installed" == "$repo_vcpkg_installed" ]]; then
    fail "refusing to use repository vcpkg_installed as Linux install root; keep Windows and Linux vcpkg trees isolated"
fi
if [[ "$VCPKG_TRIPLET" != "x64-linux-release" && "${ALLOW_NON_RELEASE_VCPKG_TRIPLET:-}" != "1" ]]; then
    fail "refusing Linux triplet '$VCPKG_TRIPLET'; use x64-linux-release to avoid Debug+Release vcpkg builds, or set ALLOW_NON_RELEASE_VCPKG_TRIPLET=1 to adopt an existing build"
fi

if [[ -d "$FAISS_ROOT" ]] &&
    { [[ ! -f "$FAISS_ROOT/include/faiss/IndexFlat.h" ]] ||
      [[ -n "$(find "$FAISS_ROOT" \( -name 'faiss.dll' -o -name 'faiss.lib' \) -type f -print -quit || true)" ]] ||
      [[ -z "$(find "$FAISS_ROOT" \( -name 'libfaiss.so*' -o -name 'libfaiss_avx2.so*' -o -name 'libfaiss_avx512.so*' \) -type f -print -quit || true)" ]]; }; then
    log "removing incomplete Faiss dependency at $FAISS_ROOT"
    rm -rf "$FAISS_ROOT"
fi

if [[ ! -d "$ONNXRUNTIME_ROOT" ]]; then
    log "downloading ONNX Runtime $ONNXRUNTIME_VERSION"
    archive="$DEPS_DIR/onnxruntime-linux-x64-$ONNXRUNTIME_VERSION.tgz"
    ensure_tgz "$ONNXRUNTIME_URL" "$archive"
    tar -xzf "$archive" -C "$DEPS_DIR"
else
    log "ONNX Runtime already present at $ONNXRUNTIME_ROOT"
fi
if [[ ! -s "$ONNXRUNTIME_ROOT/lib/libonnxruntime.so" && -s "$ONNXRUNTIME_ROOT/lib/libonnxruntime.so.$ONNXRUNTIME_VERSION" ]]; then
    log "repairing ONNX Runtime libonnxruntime.so symlink placeholder"
    cp "$ONNXRUNTIME_ROOT/lib/libonnxruntime.so.$ONNXRUNTIME_VERSION" "$ONNXRUNTIME_ROOT/lib/libonnxruntime.so"
fi

if [[ ! -d "$SQLITE_ROOT" ]]; then
    log "downloading SQLite amalgamation $SQLITE_VERSION"
    archive="$DEPS_DIR/sqlite-amalgamation-$SQLITE_VERSION.zip"
    ensure_zip "$SQLITE_URL" "$archive"
    unzip -q "$archive" -d "$DEPS_DIR"
else
    log "SQLite amalgamation already present at $SQLITE_ROOT"
fi

if [[ ! -d "$BOOST_ROOT" ]]; then
    log "downloading Boost $BOOST_VERSION"
    archive="$DEPS_DIR/boost_$BOOST_VERSION.tar.gz"
    ensure_tgz "$BOOST_URL" "$archive"
    tar -xzf "$archive" -C "$DEPS_DIR"
else
    log "Boost already present at $BOOST_ROOT"
fi

if [[ ! -d "$EIGEN_ROOT" ]]; then
    log "downloading Eigen $EIGEN_VERSION"
    archive="$DEPS_DIR/eigen-$EIGEN_VERSION.zip"
    ensure_zip "$EIGEN_URL" "$archive"
    unzip -q "$archive" -d "$DEPS_DIR"
else
    log "Eigen already present at $EIGEN_ROOT"
fi

if [[ ! -d "$FAISS_ROOT" ]]; then
    log "preparing Faiss C++ runtime"
    system_faiss="$(find /usr/lib /usr/lib64 -name 'libfaiss.so*' -type f -print -quit 2>/dev/null || true)"
    if [[ -n "$system_faiss" && -f /usr/include/faiss/IndexFlat.h ]]; then
        log "using system libfaiss-dev package from $system_faiss"
        mkdir -p "$FAISS_ROOT/include" "$FAISS_ROOT/lib"
        cp -a /usr/include/faiss "$FAISS_ROOT/include/"
        cp "$system_faiss" "$FAISS_ROOT/lib/libfaiss.so"
    else
        archive="$DEPS_DIR/libfaiss-linux.conda"
        ensure_conda "$FAISS_URL" "$archive" "$FAISS_SHA256"
        extract_conda "$archive" "$FAISS_ROOT"
    fi
else
    log "Faiss already present at $FAISS_ROOT"
fi

if [[ -n "$(find "$FAISS_ROOT" \( -name 'faiss.dll' -o -name 'faiss.lib' \) -type f -print -quit || true)" ]]; then
    log "Faiss package contains Windows binaries:"
    find "$FAISS_ROOT" \( -name 'faiss.dll' -o -name 'faiss.lib' \) -type f | sort | sed 's#^#  #'
    rm -rf "$FAISS_ROOT"
    fail "Faiss archive is not a Linux C++ package; set FAISS_URL to a linux-64 libfaiss conda package"
fi

if [[ ! -f "$FAISS_ROOT/lib/libfaiss.so" ]]; then
    found="$(find "$FAISS_ROOT" \( -name 'libfaiss.so*' -o -name 'libfaiss_avx2.so*' -o -name 'libfaiss_avx512.so*' \) -type f -print -quit || true)"
    if [[ -z "$found" && -f /usr/include/faiss/IndexFlat.h ]]; then
        found="$(find /usr/lib /usr/lib64 -name 'libfaiss.so*' -type f -print -quit 2>/dev/null || true)"
        if [[ -n "$found" ]]; then
            log "using system libfaiss-dev package from $found"
            mkdir -p "$FAISS_ROOT/include" "$FAISS_ROOT/lib"
            cp -a /usr/include/faiss "$FAISS_ROOT/include/"
            cp "$found" "$FAISS_ROOT/lib/libfaiss.so"
        fi
    fi
    if [[ -z "$found" ]]; then
        log "Faiss package contents:"
        find "$FAISS_ROOT" -maxdepth 5 \( -iname '*faiss*' -o -path '*/lib/*' \) -type f | sort | sed 's#^#  #'
        fail "Faiss shared library not found"
    fi
    if [[ ! -f "$FAISS_ROOT/lib/libfaiss.so" ]]; then
        mkdir -p "$FAISS_ROOT/lib"
        cp "$found" "$FAISS_ROOT/lib/libfaiss.so"
    fi
fi

vcpkg_baseline="$("$PYTHON_BIN" - "$REPO_ROOT/vcpkg.json" <<'PY'
import json
import pathlib
import sys
print(json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))["builtin-baseline"])
PY
 )"

if [[ ! -d "$VCPKG_ROOT/.git" ]]; then
    if [[ -d "$VCPKG_ROOT" ]] && [[ -n "$(find "$VCPKG_ROOT" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
        fail "vcpkg directory exists but is not a git checkout: $VCPKG_ROOT"
    fi
    mkdir -p "$VCPKG_ROOT"
    git -C "$VCPKG_ROOT" init
    git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git
fi

current_vcpkg_commit="$(git -C "$VCPKG_ROOT" rev-parse HEAD 2>/dev/null || true)"
if [[ "$current_vcpkg_commit" != "$vcpkg_baseline" ]]; then
    log "fetching vcpkg baseline $vcpkg_baseline"
    git -C "$VCPKG_ROOT" fetch --no-tags origin "$vcpkg_baseline"
    git -C "$VCPKG_ROOT" checkout --detach FETCH_HEAD
fi
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

triplet_file="$REPO_ROOT/triplets/ci/$VCPKG_TRIPLET.cmake"
[[ -f "$triplet_file" ]] || fail "versioned Linux triplet not found: $triplet_file"

VCPKG_MAX_CONCURRENCY="$BUILD_JOBS" "$VCPKG_ROOT/vcpkg" install \
    --triplet "$VCPKG_TRIPLET" \
    --host-triplet "$VCPKG_TRIPLET" \
    --x-feature=tests \
    --x-manifest-root="$REPO_ROOT" \
    --x-install-root="$VCPKG_INSTALL_ROOT" \
    --overlay-triplets="$REPO_ROOT/triplets/ci" \
    "--binarysource=clear;files,$VCPKG_BINARY_CACHE,readwrite" \
    --clean-after-build

if [[ -n "$LLAMA_CPP_CUDA_URL" ]] && ! has_cuda_llama_binary; then
    log "preparing CUDA llama.cpp binary from $LLAMA_CPP_CUDA_URL"
    source_archive="$DEPS_DIR/llama.cpp-$LLAMA_CPP_TAG.tar.gz"
    binary_archive="$DEPS_DIR/llama-$LLAMA_CPP_TAG-cuda.tar.gz"
    ensure_tgz "https://github.com/ggml-org/llama.cpp/archive/refs/tags/$LLAMA_CPP_TAG.tar.gz" "$source_archive"
    ensure_tgz "$LLAMA_CPP_CUDA_URL" "$binary_archive"
    rm -rf "$LLAMA_CPP_ROOT" "$DEPS_DIR/llama-cuda-bin"
    mkdir -p "$LLAMA_CPP_ROOT" "$DEPS_DIR/llama-cuda-bin"
    tar -xzf "$source_archive" -C "$LLAMA_CPP_ROOT" --strip-components=1
    tar -xzf "$binary_archive" -C "$DEPS_DIR/llama-cuda-bin"
    mkdir -p "$LLAMA_CPP_BUILD/src" "$LLAMA_CPP_BUILD/common" "$LLAMA_CPP_BUILD/tools/mtmd" "$LLAMA_CPP_BUILD/ggml/src" "$LLAMA_CPP_BUILD/bin"
    cp "$(find "$DEPS_DIR/llama-cuda-bin" -name 'libllama.so*' -type f -print -quit)" "$LLAMA_CPP_BUILD/src/libllama.so"
    cp "$(find "$DEPS_DIR/llama-cuda-bin" -name 'libmtmd.so*' -type f -print -quit)" "$LLAMA_CPP_BUILD/tools/mtmd/libmtmd.so"
    cp "$(find "$DEPS_DIR/llama-cuda-bin" -name 'libggml.so*' -type f -print -quit)" "$LLAMA_CPP_BUILD/ggml/src/libggml.so"
    cp "$(find "$DEPS_DIR/llama-cuda-bin" -name 'libggml-base.so*' -type f -print -quit)" "$LLAMA_CPP_BUILD/ggml/src/libggml-base.so"
    cp "$(find "$DEPS_DIR/llama-cuda-bin" -name 'libggml-cuda.so*' -type f -print -quit)" "$LLAMA_CPP_BUILD/ggml/src/libggml-cuda.so"
    find "$DEPS_DIR/llama-cuda-bin" -name '*.so*' -type f -exec cp {} "$LLAMA_CPP_BUILD/bin/" \;
    ar rcs "$LLAMA_CPP_BUILD/common/libllama-common.a"
fi

log "dependencies ready"
if has_cuda_llama_binary; then
    log "CUDA llama.cpp binary detected; inference target can be enabled"
else
    log "CUDA llama.cpp binary not configured; inference target will remain disabled"
fi
