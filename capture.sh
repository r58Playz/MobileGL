#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)"
backend="${MOBILEGL_BACKEND_TYPE:-DirectWebGPU}"

if [[ -n "${MOBILEGL_APITRACE:-}" ]]; then
    apitrace="$MOBILEGL_APITRACE"
elif command -v apitrace >/dev/null 2>&1; then
    apitrace="$(command -v apitrace)"
else
    apitrace="${repo_dir}/build-dawn/apitrace/apitrace"
fi

if [[ ! -x "$apitrace" ]]; then
    printf 'apitrace executable not found: %s\n' "$apitrace" >&2
    printf 'Install system apitrace, run tools/native_harness/build_all.sh, or set MOBILEGL_APITRACE.\n' >&2
    exit 1
fi

if [[ -n "${MOBILEGL_TRACE_OUTPUT:-}" ]]; then
    output="$MOBILEGL_TRACE_OUTPUT"
    if [[ "$output" != /* ]]; then
        output="${repo_dir}/${output}"
    fi
else
    stamp="$(date +%Y%m%d-%H%M%S)"
    output="${repo_dir}/captures/minecraft-${backend}-${stamp}.trace"
fi
mkdir -p -- "$(dirname -- "$output")"

printf '[mobilegl-capture] backend=%s\n' "$backend" >&2
printf '[mobilegl-capture] output=%s\n' "$output" >&2

exec env MOBILEGL_BACKEND_TYPE="$backend" \
    "${repo_dir}/launch.sh" \
    "$apitrace" trace --api=gl --output "$output" -- "$@"
