#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_dir="$(cd -- "${script_dir}/../.." && pwd)"
build_dir="${MOBILEGL_BUILD_DIR:-${source_dir}/build-dawn}"
jobs="${MOBILEGL_BUILD_JOBS:-}"
dawn_source="${MOBILEGL_DAWN_SOURCE_DIR:-}"
vulkan_build_dir="${MOBILEGL_VULKAN_BUILD_DIR:-${source_dir}/build-vulkan}"
build_bundled_apitrace="${MOBILEGL_BUILD_BUNDLED_APITRACE:-auto}"

usage() {
    printf '%s\n' \
        "Usage: $0 [--build-dir PATH] [--jobs N] [--dawn-source PATH]" \
        "" \
        "Builds the WebGPU and Vulkan MobileGL backends, GLX bridges, trace replay, and SFPEW." \
        "Uses system apitrace when available; set MOBILEGL_BUILD_BUNDLED_APITRACE=ON to build it." \
        "The default output directory is: ${source_dir}/build-dawn"
}

while (($#)); do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --jobs)
            jobs="$2"
            shift 2
            ;;
        --dawn-source)
            dawn_source="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

for tool in cmake ninja clang clang++; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Required build tool not found: %s\n' "$tool" >&2
        exit 1
    fi
done

# Dawn's monolithic ThinLTO link can open more than the common 1024-file soft
# limit. Raising only this process' soft limit avoids a late link failure.
if [[ "$(ulimit -Sn)" =~ ^[0-9]+$ ]] && (( $(ulimit -Sn) < 65536 )); then
    ulimit -Sn 65536 2>/dev/null || true
fi

if [[ -z "$jobs" ]]; then
    jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '4')"
fi
if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    printf '%s\n' '--jobs must be a positive integer' >&2
    exit 2
fi

cmake_args=(
    -S "$source_dir"
    -B "$build_dir"
    -G Ninja
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_C_COMPILER=clang
    -DCMAKE_CXX_COMPILER=clang++
    -DCMAKE_CXX_FLAGS=-Wno-c2y-extensions
    -DMOBILEGL_NATIVE_WEBGPU=ON
    -DMOBILEGL_BUILD_NATIVE_HARNESS=ON
    -DMOBILEGL_BUILD_TRACE_REPLAY=ON
    -DMOBILEGL_BUILD_TEST=OFF
    -DMOBILEGL_BUILD_BENCHMARK=OFF
)
if [[ -n "$dawn_source" ]]; then
    cmake_args+=("-DMOBILEGL_DAWN_SOURCE_DIR=${dawn_source}")
fi

printf 'Configuring MobileGL/Dawn in %s\n' "$build_dir"
cmake "${cmake_args[@]}"
cmake --build "$build_dir" --parallel "$jobs" --target \
    MobileGL mobilegl_glx_bridge mobilegl_trace_replay

printf 'Configuring DirectVulkan reference build in %s\n' "$vulkan_build_dir"
cmake \
    -S "$source_dir" \
    -B "$vulkan_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS=-Wno-c2y-extensions \
    -DMOBILEGL_NATIVE_WEBGPU=OFF \
    -DMOBILEGL_BUILD_NATIVE_HARNESS=ON \
    -DMOBILEGL_BUILD_TRACE_REPLAY=ON \
    -DMOBILEGL_BUILD_TEST=OFF \
    -DMOBILEGL_BUILD_BENCHMARK=OFF
cmake --build "$vulkan_build_dir" --parallel "$jobs" --target \
    MobileGL mobilegl_glx_bridge mobilegl_trace_replay

sfpew_build_dir="${build_dir}/sfpew"
printf 'Configuring SFPEW in %s\n' "$sfpew_build_dir"
cmake \
    -S "${source_dir}/SimpleFPEWrapper" \
    -B "$sfpew_build_dir" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DSFPEW_BUILD_TESTS=OFF
cmake --build "$sfpew_build_dir" --parallel "$jobs" --target SimpleFPEWrapper

apitrace_build_dir="${build_dir}/apitrace"
use_bundled_apitrace=0
if [[ "$build_bundled_apitrace" == "1" || "$build_bundled_apitrace" == "ON" ]]; then
    use_bundled_apitrace=1
elif [[ "$build_bundled_apitrace" == "auto" ]] && ! command -v apitrace >/dev/null 2>&1; then
    use_bundled_apitrace=1
fi

if ((use_bundled_apitrace)); then
    printf 'Configuring apitrace capture tools in %s\n' "$apitrace_build_dir"
    cmake \
        -S "${source_dir}/3rdparty/apitrace" \
        -B "$apitrace_build_dir" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_GUI=OFF \
        -DBUILD_TESTING=OFF
    cmake --build "$apitrace_build_dir" --parallel "$jobs" --target apitrace gltrim
    apitrace_path="${apitrace_build_dir}/apitrace"
    gltrim_path="${apitrace_build_dir}/gltrim"
else
    apitrace_path="$(command -v apitrace 2>/dev/null || true)"
    gltrim_path="system apitrace gltrim subcommand"
    printf 'Using system apitrace: %s\n' "$apitrace_path"
fi

printf '\nBuild complete:\n'
printf '  MobileGL:    %s\n' "${build_dir}/libMobileGL.so"
printf '  GLX bridge:  %s\n' "${build_dir}/tools/native_harness/libMobileGLGLX.so"
printf '  trace replay:%s\n' " ${build_dir}/tools/trace_replay/mobilegl_trace_replay"
printf '  Vulkan MobileGL:    %s\n' "${vulkan_build_dir}/libMobileGL.so"
printf '  Vulkan GLX bridge:  %s\n' "${vulkan_build_dir}/tools/native_harness/libMobileGLGLX.so"
printf '  Vulkan trace replay:%s\n' " ${vulkan_build_dir}/tools/trace_replay/mobilegl_trace_replay"
printf '  SFPEW:       %s\n' "${sfpew_build_dir}/libSimpleFPEWrapper.so"
printf '  apitrace:    %s\n' "$apitrace_path"
printf '  gltrim:      %s\n' "$gltrim_path"
printf '\nSee tools/native_harness/README.md for launch and replay commands.\n'
