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
# Force the GL clients onto X11: the GLX bridge only implements the X11/GLX path.
# WAYLAND_DISPLAY must be UNSET, not merely emptied — GLFW 3.4 (used by
# lwjgl3ify/GTNH via -Dorg.lwjgl.glfw.libname=/usr/lib/libglfw.so) selects the
# Wayland backend whenever the variable is *present* (even empty), then aborts with
# "Wayland: Failed to connect to display" instead of falling back to X11. SDL
# tolerates the empty value, which is why only the GLFW clients broke.
env -u WAYLAND_DISPLAY python3 tools/native_harness/launch_prism.py "${launcher_args[@]}" -- "$@"
