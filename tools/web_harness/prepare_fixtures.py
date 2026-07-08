#!/usr/bin/env python3
"""Extract all trace_replay fixtures into a servable dir and emit a run manifest.

Reads tools/trace_replay/trace_cases.json, extracts each case's trace file from its
.tgz into <out>/rep/<name>.trace, copies the golden PNG next to it, and writes
<out>/rep/manifest.json describing the query params each case needs. This lets the
runtime-fetch web harness replay every fixture from a single build.
"""
import json, os, sys, tarfile, shutil, argparse

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIX = os.path.join(REPO, "tools", "trace_replay", "fixtures")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "build-web", "rep"))
    ap.add_argument("--only", default="", help="comma-separated case names to extract")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    cases = json.load(open(os.path.join(REPO, "tools", "trace_replay", "trace_cases.json")))
    defaults = cases["defaults"]
    only = set(x for x in args.only.split(",") if x)
    manifest = []
    for c in cases["cases"]:
        name = c["name"]
        if only and name not in only:
            continue
        arch = os.path.join(FIX, c["trace_archive"])
        trace_member = c.get("trace_file", defaults["trace_file"])
        out_trace = os.path.join(args.out, name + ".trace")
        if not os.path.exists(out_trace):
            with tarfile.open(arch) as tf:
                member = tf.getmember(trace_member)
                with tf.extractfile(member) as src, open(out_trace, "wb") as dst:
                    shutil.copyfileobj(src, dst, length=8 * 1024 * 1024)
        golden_src = os.path.join(FIX, c["golden"])
        golden_out = name + ".golden.png"
        shutil.copyfile(golden_src, os.path.join(args.out, golden_out))
        entry = {
            "name": name,
            "trace": "rep/" + name + ".trace",
            "golden": "rep/" + golden_out,
            "target": c["target_call"],
            "width": c.get("width", defaults["width"]),
            "height": c.get("height", defaults["height"]),
        }
        if "alternate_golden" in c:
            alt = name + ".golden-alt.png"
            shutil.copyfile(os.path.join(FIX, c["alternate_golden"]), os.path.join(args.out, alt))
            entry["alternate_golden"] = "rep/" + alt
        manifest.append(entry)
        print(f"prepared {name}: target={entry['target']} {entry['width']}x{entry['height']}", flush=True)
    json.dump(manifest, open(os.path.join(args.out, "manifest.json"), "w"), indent=2)
    print(f"\nwrote {len(manifest)} cases to {args.out}/manifest.json")

if __name__ == "__main__":
    main()
