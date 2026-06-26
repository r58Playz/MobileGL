#!/usr/bin/env bash
# MobileGL standalone DirectWebGPU web harness — reproducible build.
#
# Builds libMobileGL.a (wasm) + the mobilegl_web_harness.html/.js/.wasm bundle that
# drives the real GL/EGL frontend against the DirectWebGPU backend (clear, draws,
# textures/samplers, and JSPI readback). Serve the output dir over HTTP and open
# mobilegl_web_harness.html in a WebGPU-capable browser.
#
# Prerequisites:
#   1. The Emscripten SDK (3.1.56, matching .NET 10) sourced into the env so that
#      `emcmake`/`emcc` are on PATH:   source /path/to/emsdk/emsdk_env.sh
#   2. tint prebuilt to tools/tint/out (libtint.a + headers):  tools/build-tint.sh
#   3. glslang's SPIRV-Tools populated once:  python3 3rdparty/glslang/update_glslang_sources.py
#
# Usage:
#   tools/web_harness/build.sh [build-dir]      (default build dir: ./build-web)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build-web}"
TINT_DIR="${MOBILEGL_TINT_DIR:-${REPO_ROOT}/tools/tint/out}"
JOBS="$(nproc 2>/dev/null || echo 4)"

if ! command -v emcmake >/dev/null 2>&1; then
  echo "error: emcmake not found. Source the emsdk env first:" >&2
  echo "       source /path/to/emsdk/emsdk_env.sh" >&2
  exit 1
fi

if [ ! -f "${TINT_DIR}/libtint.a" ]; then
  echo "error: ${TINT_DIR}/libtint.a not found. Build tint first:" >&2
  echo "       tools/build-tint.sh" >&2
  exit 1
fi

echo "==> Configuring (build dir: ${BUILD_DIR})"
emcmake cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DMOBILEGL_FORCE_RELEASE_OPT=ON \
  -DMOBILEGL_BUILD_WEB_HARNESS=ON \
  -DMOBILEGL_TINT_DIR="${TINT_DIR}"

echo "==> Building mobilegl_web_harness"
cmake --build "${BUILD_DIR}" --target mobilegl_web_harness -j"${JOBS}"

echo
echo "==> Done. Serve it and open the harness:"
echo "    (cd '${BUILD_DIR}' && python3 -m http.server 8105)"
echo "    then browse http://localhost:8105/mobilegl_web_harness.html"
