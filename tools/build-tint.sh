#!/usr/bin/env bash
# build-tint.sh - Builds Dawn's Tint (SPIR-V -> WGSL) as a single WASM static
# archive (libtint.a) + a header bundle, for the MobileGL DirectWebGPU backend.
#
# Mirrors tools/native-deps/build-glfw.sh: produces prebuilt artifacts that the
# MobileGL/harness build links against. Nothing from Dawn is committed; only this
# script + tools/tint/dawn-tint-emscripten.patch live in the tree.
#
# Output (in OUTPUT_DIR, default tools/tint/out):
#   libtint.a         - combined Tint + Abseil + SPIRV-Tools objects
#   include/          - headers: include root for "src/tint/..." and "absl/..."
#   tint.flags        - reminder of the required consumer compile/link flags
#
# Requires: emsdk (emcmake/emcc/emar) on PATH, git, python3.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Pin the SAME Dawn revision as the vendored emdawnwebgpu package so the WGSL that
# Tint emits and the emdawnwebgpu bindings agree.
DAWN_REPO="${DAWN_REPO:-https://dawn.googlesource.com/dawn}"
DAWN_REF="${DAWN_REF:-b465e23d19a5e27402d80191c0f8de1fb3b385b5}"
DAWN_PATCH="${DAWN_PATCH:-$SCRIPT_DIR/tint/dawn-tint-emscripten.patch}"
OUTPUT_DIR="${OUTPUT_DIR:-$SCRIPT_DIR/tint/out}"
TMP_DIR="${TMP_DIR:-}"
KEEP_TMP="${KEEP_TMP:-false}"
JOBS="${JOBS:-$(nproc)}"

log() { echo "[build-tint] $*"; }
require() { command -v "$1" >/dev/null 2>&1 || { echo "ERROR: '$1' not found" >&2; exit 1; }; }
require emcmake; require emcc; require emar; require git; require python3

if [ -z "$TMP_DIR" ]; then
    TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mgl-tint-build.XXXXXX")"
fi
DAWN_SRC="$TMP_DIR/dawn"
DAWN_BUILD="$TMP_DIR/dawn-build"
cleanup() { if [ "$KEEP_TMP" != "true" ]; then rm -rf "$TMP_DIR"; fi; }
trap cleanup EXIT

# ----------------------------------------------------------------------------
log "Fetching Dawn @ $DAWN_REF (shallow)"
mkdir -p "$DAWN_SRC"
git -C "$DAWN_SRC" init -q
git -C "$DAWN_SRC" remote add origin "$DAWN_REPO" 2>/dev/null || true
git -C "$DAWN_SRC" fetch -q --depth 1 origin "$DAWN_REF"
git -C "$DAWN_SRC" checkout -q FETCH_HEAD

log "Applying tint-only Emscripten patch"
git -C "$DAWN_SRC" apply "$DAWN_PATCH"

log "Fetching Dawn dependencies (shallow)"
python3 "$DAWN_SRC/tools/fetch_dawn_dependencies.py" -d "$DAWN_SRC" -s >/dev/null

# Unify SPIRV-Tools/SPIRV-Headers with MobileGL's glslang (3rdparty/glslang/known_good.json)
# so tint's bundled copy is byte-identical to MobileGL's. Without this, the final link
# has two different SPIRV-Tools and tint's reader binds to glslang's incompatible copy
# (heap OOB in ast_parser). Pin from the canonical Khronos remotes.
SPIRV_TOOLS_REV="${SPIRV_TOOLS_REV:-33e02568181e3312f49a3cf33df470bf96ef293a}"
SPIRV_HEADERS_REV="${SPIRV_HEADERS_REV:-2a611a970fdbc41ac2e3e328802aed9985352dca}"
# fetch_dawn_dependencies leaves the deps without their own .git, so re-pin by a
# fresh shallow clone into a new repo at the exact revision.
repin_dep() {  # <dir> <url> <rev>
    rm -rf "$1"; mkdir -p "$1"
    git -C "$1" init -q
    git -C "$1" fetch -q --depth 1 "$2" "$3"
    git -C "$1" checkout -q FETCH_HEAD
}
# Dawn expects the sources under <dep>/src (DAWN_SPIRV_TOOLS_DIR=.../spirv-tools/src).
log "Re-pinning SPIRV-Tools -> $SPIRV_TOOLS_REV (match glslang)"
repin_dep "$DAWN_SRC/third_party/spirv-tools/src" https://github.com/KhronosGroup/SPIRV-Tools "$SPIRV_TOOLS_REV"
log "Re-pinning SPIRV-Headers -> $SPIRV_HEADERS_REV (match glslang)"
repin_dep "$DAWN_SRC/third_party/spirv-headers/src" https://github.com/KhronosGroup/SPIRV-Headers "$SPIRV_HEADERS_REV"

# ----------------------------------------------------------------------------
log "Configuring Tint-only build (backends off; SPV reader + WGSL writer on)"
emcmake cmake -S "$DAWN_SRC" -B "$DAWN_BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF \
    -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF \
    -DDAWN_ENABLE_NULL=OFF -DDAWN_ENABLE_SWIFTSHADER=OFF \
    -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF \
    -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF \
    -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_WGSL_READER=ON \
    -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF \
    -DTINT_BUILD_MSL_WRITER=OFF -DTINT_BUILD_HLSL_WRITER=OFF -DTINT_BUILD_SPV_WRITER=OFF \
    -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
    -DDAWN_ENABLE_INSTALL=OFF -DTINT_ENABLE_INSTALL=OFF >/dev/null

log "Building tint_api (this also builds Abseil + SPIRV-Tools)"
cmake --build "$DAWN_BUILD" --target tint_api -j"$JOBS" >/dev/null

# ----------------------------------------------------------------------------
log "Combining archives into a single libtint.a"
rm -rf "$OUTPUT_DIR"; mkdir -p "$OUTPUT_DIR/include"
mapfile -t ARCHIVES < <(
    find "$DAWN_BUILD/src/tint" -name 'libtint_*.a'
    find "$DAWN_BUILD" -name 'libabsl_*.a'
    find "$DAWN_BUILD/third_party/spirv-tools" -name 'libSPIRV-Tools*.a'
)
log "  ${#ARCHIVES[@]} archives -> $OUTPUT_DIR/libtint.a"
{
    echo "create $OUTPUT_DIR/libtint.a"
    for a in "${ARCHIVES[@]}"; do echo "addlib $a"; done
    echo "save"
    echo "end"
} | emar -M
emar t "$OUTPUT_DIR/libtint.a" >/dev/null   # sanity

log "Bundling headers (src/ for \"src/tint/...\", absl/ for \"absl/...\")"
( cd "$DAWN_SRC" && find src \( -name '*.h' -o -name '*.inc' \) | tar -cf - -T - ) | tar -xf - -C "$OUTPUT_DIR/include"
( cd "$DAWN_SRC/third_party/abseil-cpp" && find absl \( -name '*.h' -o -name '*.inc' \) | tar -cf - -T - ) | tar -xf - -C "$OUTPUT_DIR/include"

cat > "$OUTPUT_DIR/tint.flags" <<EOF
# Consumer requirements for libtint.a (Dawn $DAWN_REF):
#   compile the tint-using TU with: -std=c++20 -fno-rtti -fno-exceptions
#   include dir: -I "$OUTPUT_DIR/include"
#   defines: -DTINT_BUILD_SPV_READER=1 -DTINT_BUILD_WGSL_READER=1 -DTINT_BUILD_WGSL_WRITER=1 \\
#            -DTINT_BUILD_SPV_WRITER=0 -DTINT_BUILD_GLSL_WRITER=0 -DTINT_BUILD_HLSL_WRITER=0 \\
#            -DTINT_BUILD_MSL_WRITER=0 -DTINT_BUILD_IR_BINARY=0 \\
#            -DTINT_BUILD_IS_LINUX=1 -DTINT_BUILD_IS_MAC=0 -DTINT_BUILD_IS_WIN=0
#   FINAL LINK must set a large stack: -sSTACK_SIZE=16MB (tint AST passes recurse deeply)
EOF

log "Done:"
ls -la "$OUTPUT_DIR"
log "libtint.a size: $(du -h "$OUTPUT_DIR/libtint.a" | cut -f1)"
