# Linux local build

This directory controls a Linux or WSL2 build without changing the root Windows-oriented CMake file.
The scripts create a generated Linux source tree under `build/linux-source`, apply the same Linux
dependency shim used by CI, and build Linux ELF test artifacts.

The default flow does not build the full multimodal inference server because llama.cpp must use a
CUDA binary release. It does build the CPU BERT emotion server, Persona service tests, document/vector
tests, the Persona Gateway E2E server, and the formal Gateway server.

```bash
bash linux/scripts/bootstrap_toolchain.sh
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh
bash linux/scripts/build.sh
bash linux/scripts/check_artifacts.sh
bash linux/scripts/test.sh
```

Create a deployable runtime directory after the build succeeds:

```bash
bash linux/scripts/package.sh
```

The package is written to `build/linux-package` by default. It contains the built ELF binaries,
runtime shared libraries discovered with `ldd`, config templates, and the `dist` frontend assets
when present. Override the output directory with `PACKAGE_DIR=/path/to/package`.

The same flow is available through the control CMake project:

```bash
cmake -S linux -B build/linux-control -G Ninja
cmake --build build/linux-control --target linux_all
```

To enable the inference target, provide a compatible Linux CUDA llama.cpp release archive:

```bash
export LLAMA_CPP_CUDA_URL="https://.../llama-<tag>-bin-ubuntu-cuda-<version>-x64.tar.gz"
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh --inference
bash linux/scripts/build.sh --inference
bash linux/scripts/check_artifacts.sh --inference
```

The CUDA archive must contain `libllama.so`, `libmtmd.so`, `libggml.so`, `libggml-base.so`, and
`libggml-cuda.so`. The scripts consume prebuilt binaries and do not install or invoke a CUDA toolkit.

When using the current Windows checkout from WSL2, enter it through `/mnt`, for example:

```bash
cd /path/to/AgentLoom
bash linux/scripts/prepare_deps.sh
bash linux/scripts/configure.sh
bash linux/scripts/build.sh
bash linux/scripts/check_artifacts.sh
```

For better WSL2 filesystem performance, clone or copy the repository into a Linux filesystem path:

```bash
mkdir -p ~/repos
cd ~/repos
git clone <repo-url> AgentLoom
cd AgentLoom
```

The scripts derive paths from the current checkout root, so both `/mnt/d/...` and `~/repos/...` work.

`bootstrap_toolchain.sh` creates a private Python virtual environment at `build/linux-python-venv`.
This avoids Ubuntu/WSL externally-managed Python restrictions and keeps `zstandard` out of the
system Python installation. Rust uses the repository `rust-toolchain.toml`, matching CI's stable,
minimal toolchain with `rustfmt` and `clippy` components.

gRPC is still built through vcpkg for ABI consistency, but the scripts use a release-only triplet,
use the same release-only triplet for host code-generation tools,
an isolated Linux install root at `build/linux-vcpkg-installed`, and a local vcpkg binary cache at
`build/vcpkg-binary-cache`. The isolated install root keeps WSL/Linux package state separate from
the Windows `vcpkg_installed` tree. The first gRPC build can still be slow; later runs should restore
the cached package instead of rebuilding it. The versioned triplet lives at
`triplets/ci/x64-linux-release.cmake`; do not replace it with a generated or machine-local file.
The package-consumer verification also receives the prepared Boost 1.85 source root explicitly, so
it validates the same public-header ABI used to build the SDK instead of resolving another Boost.
Increase parallelism on a larger machine with:

```bash
BUILD_JOBS=4 bash linux/scripts/prepare_deps.sh
```

`test.sh` runs the default Linux test set and filters out CTest placeholders for targets that were
not built plus Redis-backed reload tests. Use explicit flags when the matching dependency is
available:

```bash
bash linux/scripts/test.sh --redis
bash linux/scripts/test.sh --all
```

If a previous WSL run already spent time building the default `x64-linux` triplet, it can be adopted
explicitly instead of discarded:

```bash
ALLOW_NON_RELEASE_VCPKG_TRIPLET=1 VCPKG_TRIPLET=x64-linux bash linux/scripts/prepare_deps.sh
ALLOW_NON_RELEASE_VCPKG_TRIPLET=1 VCPKG_TRIPLET=x64-linux bash linux/scripts/configure.sh
```

Use this only to reuse an existing build. New local Linux builds should keep the default
`x64-linux-release` triplet to avoid building both Debug and Release packages.
