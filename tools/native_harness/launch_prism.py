#!/usr/bin/env python3
"""Launch Prism (or a direct Java command) with the MobileGL GLX bridge."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bridge", required=True, type=Path)
    parser.add_argument("--mobilegl", required=True, type=Path)
    parser.add_argument("--backend", choices=("DirectVulkan", "DirectWebGPU"),
                        default=os.environ.get("MOBILEGL_BACKEND_TYPE", "DirectWebGPU"))
    parser.add_argument("--frontend", choices=("core", "sfpew"), default="core")
    parser.add_argument("--sfpew", type=Path)
    parser.add_argument("--mobilegl-log", type=Path)
    parser.add_argument("--trace", action="store_true", help="log bridge interception events")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("a Prism/Java command is required after --")
    if args.frontend == "sfpew" and not args.sfpew:
        parser.error("--sfpew is required with --frontend sfpew")
    for path in (args.bridge, args.mobilegl, args.sfpew):
        if path and not path.is_file():
            parser.error(f"library does not exist: {path}")

    env = os.environ.copy()
    bridge = args.bridge.resolve()
    mobilegl = args.mobilegl.resolve()
    preload = str(bridge)
    if env.get("LD_PRELOAD"):
        preload += ":" + env["LD_PRELOAD"]
    library_path = f"{bridge.parent}:{mobilegl.parent}"
    if env.get("LD_LIBRARY_PATH"):
        library_path += ":" + env["LD_LIBRARY_PATH"]
    env.update({
        "LD_PRELOAD": preload,
        "LD_LIBRARY_PATH": library_path,
        "MOBILEGL_LIBRARY": str(mobilegl),
        "MOBILEGL_BACKEND_TYPE": args.backend,
        "MOBILEGL_FRONTEND": args.frontend,
        # Minecraft 26.2 uses LWJGL's SDL3 backend. SDL can independently load
        # either GLX/OpenGL or EGL, so pin both loader paths to this harness.
        "SDL_OPENGL_LIBRARY": str(bridge),
        "SDL_EGL_LIBRARY": str(mobilegl),
        "SDL_VIDEO_FORCE_EGL": "0",
    })
    if args.trace:
        env["MOBILEGL_GLX_TRACE"] = "1"
        print(f"[mobilegl-launch] LD_PRELOAD={env['LD_PRELOAD']}", file=sys.stderr, flush=True)
        print(f"[mobilegl-launch] LD_LIBRARY_PATH={env['LD_LIBRARY_PATH']}", file=sys.stderr, flush=True)
        print(f"[mobilegl-launch] MOBILEGL_BACKEND_TYPE={env['MOBILEGL_BACKEND_TYPE']}",
              file=sys.stderr, flush=True)
        print(f"[mobilegl-launch] SDL_OPENGL_LIBRARY={env['SDL_OPENGL_LIBRARY']}",
              file=sys.stderr, flush=True)
        print(f"[mobilegl-launch] SDL_EGL_LIBRARY={env['SDL_EGL_LIBRARY']}",
              file=sys.stderr, flush=True)
        print(f"[mobilegl-launch] SDL_VIDEO_FORCE_EGL={env['SDL_VIDEO_FORCE_EGL']}",
              file=sys.stderr, flush=True)
    if args.sfpew:
        env["SFPEW_LIBRARY"] = str(args.sfpew.resolve())
        env["SFPEW_DEFER_INIT"] = "1"
    if args.mobilegl_log:
        env["MOBILEGL_LOG_FILE_PATH"] = str(args.mobilegl_log.resolve())
        if args.trace:
            print(f"[mobilegl-launch] MOBILEGL_LOG_FILE_PATH={env['MOBILEGL_LOG_FILE_PATH']}",
                  file=sys.stderr, flush=True)
    return subprocess.run(command, env=env, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
