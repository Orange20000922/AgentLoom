#!/usr/bin/env bash
set -euo pipefail

deps="$1"
version="${2:-1.17.1}"
root="$deps/onnxruntime-linux-x64-$version"
archive="$deps/microsoft.ml.onnxruntime.$version.zip"
url="https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$version/microsoft.ml.onnxruntime.$version.nupkg"

# 兼容已解压的 Linux 发布包及从 Windows 迁移时丢失的无版本库链接。
if [[ -s "$root/include/onnxruntime_cxx_api.h" && -s "$root/lib/libonnxruntime.so.$version" ]]; then
    if [[ ! -s "$root/lib/libonnxruntime.so" ]]; then
        ln -sfn "libonnxruntime.so.$version" "$root/lib/libonnxruntime.so"
    fi
    exit 0
fi

mkdir -p "$deps"
stage="$(mktemp -d "$deps/.ort-XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT

valid_archive() {
    if [[ "$version" == "1.17.1" ]]; then
        printf '%s  %s\n' '60493669633f99542a0a3e4dd17bfa9542cbfcd9852ef672f8c36ce6a9d2d935' "$1" | sha256sum --check --status
    else
        unzip -tq "$1" >/dev/null
    fi
}

if [[ ! -s "$archive" ]] || ! valid_archive "$archive"; then
    echo "Downloading $url"
    curl --fail --location --retry 3 "$url" -o "$stage/package.zip"
    valid_archive "$stage/package.zip" || { echo "Invalid ONNX Runtime package" >&2; exit 1; }
    mv "$stage/package.zip" "$archive"
fi

# NuGet 将公共头文件与平台库分开打包。恢复 CMake 和运行时 SONAME 使用的目录与文件名。
mkdir -p "$stage/include" "$stage/lib"
unzip -jq "$archive" 'build/native/include/*.h' -d "$stage/include"
unzip -p "$archive" runtimes/linux-x64/native/libonnxruntime.so > "$stage/lib/libonnxruntime.so.$version"
test -s "$stage/include/onnxruntime_cxx_api.h"
test -s "$stage/lib/libonnxruntime.so.$version"
mkdir -p "$root/include" "$root/lib"
cp "$stage/include/"*.h "$root/include/"
mv "$stage/lib/libonnxruntime.so.$version" "$root/lib/"
ln -sfn "libonnxruntime.so.$version" "$root/lib/libonnxruntime.so"
