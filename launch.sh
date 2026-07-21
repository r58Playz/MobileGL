#!/bin/bash

cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" || exit 1
echo "$@"
backend="${MOBILEGL_BACKEND_TYPE:-DirectWebGPU}"
frontend="${MOBILEGL_FRONTEND:-sfpew}"
sfpew_library="${MOBILEGL_SFPEW_LIBRARY:-build-dawn/sfpew/libSimpleFPEWrapper.so}"
if [[ -n "${MOBILEGL_BUILD_DIR:-}" ]]; then
    build_dir="$MOBILEGL_BUILD_DIR"
elif [[ "$backend" == "DirectVulkan" ]]; then
    build_dir="build-vulkan"
else
    build_dir="build-dawn"
fi
launcher_args=(
    --bridge "$build_dir/tools/native_harness/libMobileGLGLX.so"
    --mobilegl "$build_dir/libMobileGL.so"
    --backend "$backend"
    --frontend "$frontend"
    --mobilegl-log "$build_dir/mobilegl-runtime.log"
    --trace
)
if [[ "$frontend" == "sfpew" ]]; then
    launcher_args+=(--sfpew "$sfpew_library")
fi
WAYLAND_DISPLAY= python3 tools/native_harness/launch_prism.py "${launcher_args[@]}" -- "$@"
