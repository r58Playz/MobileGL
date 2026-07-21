#!/usr/bin/env python3
"""Compare two renderer frames using the MobileGL differential gates."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def global_ssim(a: np.ndarray, b: np.ndarray) -> float:
    """Channel-averaged SSIM over the full image (stable and dependency-free)."""
    if a.shape != b.shape:
        return 0.0
    scores = []
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    for channel in range(a.shape[2]):
        x = a[:, :, channel]
        y = b[:, :, channel]
        mean_x = x.mean()
        mean_y = y.mean()
        var_x = ((x - mean_x) ** 2).mean()
        var_y = ((y - mean_y) ** 2).mean()
        covariance = ((x - mean_x) * (y - mean_y)).mean()
        numerator = (2 * mean_x * mean_y + c1) * (2 * covariance + c2)
        denominator = (mean_x**2 + mean_y**2 + c1) * (var_x + var_y + c2)
        scores.append(float(numerator / denominator))
    return float(np.mean(scores))


def compare(reference: Path, candidate: Path, delta_threshold: int = 16) -> dict:
    ref = load_rgb(reference)
    actual = load_rgb(candidate)
    if ref.shape != actual.shape:
        return {
            "ssim": 0.0,
            "changed_pixel_fraction": 1.0,
            "max_channel_delta": 255,
            "shape_reference": list(ref.shape),
            "shape_candidate": list(actual.shape),
        }
    delta = np.abs(ref - actual)
    changed = np.any(delta > delta_threshold, axis=2)
    return {
        "ssim": global_ssim(ref, actual),
        "changed_pixel_fraction": float(changed.mean()),
        "max_channel_delta": int(delta.max()),
        "shape_reference": list(ref.shape),
        "shape_candidate": list(actual.shape),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--ssim", type=float, default=0.99)
    parser.add_argument("--changed-fraction", type=float, default=0.01)
    parser.add_argument("--delta", type=int, default=16)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--diff", type=Path)
    args = parser.parse_args()

    result = compare(args.reference, args.candidate, args.delta)
    result["pass"] = (
        result["ssim"] >= args.ssim
        and result["changed_pixel_fraction"] <= args.changed_fraction
    )
    result["thresholds"] = {
        "ssim": args.ssim,
        "changed_pixel_fraction": args.changed_fraction,
        "channel_delta": args.delta,
    }
    encoded = json.dumps(result, indent=2, sort_keys=True)
    print(encoded)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(encoded + "\n")
    if args.diff:
        args.diff.parent.mkdir(parents=True, exist_ok=True)
        ImageChops.difference(
            Image.open(args.reference).convert("RGB"),
            Image.open(args.candidate).convert("RGB"),
        ).save(args.diff)
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
