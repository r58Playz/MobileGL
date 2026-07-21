#!/usr/bin/env python3
"""Replay a trace through two backends, compare it, and localize the first bad call."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
from pathlib import Path

from compare_frames import compare


BAD_LOG_MARKERS = (
    "validation error",
    "device lost",
    "wgpuerror",
    "mobilegl_assert",
    "segmentation fault",
)


def render(command: str, trace: Path, output_dir: Path, call: int) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    log = output_dir / "harness.log"
    argv = shlex.split(command.format(trace=trace, output=output_dir, call=call))
    completed = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    log.write_text(completed.stdout)
    if completed.returncode:
        raise RuntimeError(f"renderer exited {completed.returncode}; see {log}")
    log_text = completed.stdout
    for renderer_log in output_dir.glob("*.log"):
        if renderer_log != log:
            log_text += "\n" + renderer_log.read_text(errors="replace")
    lowered = log_text.lower()
    marker = next((value for value in BAD_LOG_MARKERS if value in lowered), None)
    if marker:
        raise RuntimeError(f"renderer log contains {marker!r}; see {output_dir}")
    output = output_dir / "actual.png"
    if not output.is_file():
        raise RuntimeError(f"renderer did not create {output}")
    return output


def is_match(reference: Path, candidate: Path, ssim: float, fraction: float, delta: int) -> tuple[bool, dict]:
    metrics = compare(reference, candidate, delta)
    passed = metrics["ssim"] >= ssim and metrics["changed_pixel_fraction"] <= fraction
    return passed, metrics


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--reference-command", required=True,
                        help="argv template containing {trace}, {output}, and {call}")
    parser.add_argument("--candidate-command", required=True)
    parser.add_argument("--target-call", required=True, type=int)
    parser.add_argument("--first-call", type=int, default=1)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--ssim", type=float, default=0.99)
    parser.add_argument("--changed-fraction", type=float, default=0.01)
    parser.add_argument("--delta", type=int, default=16)
    parser.add_argument("--localize", action="store_true")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    cache: dict[tuple[str, int], Path] = {}

    def run(which: str, call: int) -> Path:
        key = (which, call)
        if key in cache:
            return cache[key]
        output_dir = args.output / f"{which}-{call}"
        command = args.reference_command if which == "reference" else args.candidate_command
        image = render(command, args.trace, output_dir, call)
        cache[key] = image
        return image

    def compare_call(call: int) -> tuple[bool, dict]:
        return is_match(run("reference", call), run("candidate", call),
                        args.ssim, args.changed_fraction, args.delta)

    passed, metrics = compare_call(args.target_call)
    report = {"target_call": args.target_call, "target_metrics": metrics, "pass": passed}
    if not passed and args.localize:
        low = args.first_call
        high = args.target_call
        # This assumes a persistent divergence. The retained midpoint images make a
        # non-monotonic failure obvious and allow a later pass-aware refinement.
        while low < high:
            middle = (low + high) // 2
            middle_passed, _ = compare_call(middle)
            if middle_passed:
                low = middle + 1
            else:
                high = middle
        first_pass, first_metrics = compare_call(low)
        report["first_bad_call"] = None if first_pass else low
        report["first_bad_metrics"] = first_metrics

    report_path = args.output / "report.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
