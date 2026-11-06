#!/usr/bin/env python3
"""Rendering health report for a frame dump and its emulator log.

  python -I tools/nx2/render_health.py <capture dir> [<eden_log.txt>]

Counts the signs of graphics that are not emulated correctly: rejected or dropped shaders and
draws, NaN/Inf or constant (flat) pass outputs, draws with zero texture handles, images the
allocator could not back, unimplemented service calls and device errors. Prints one line per
check and the worst offenders. A clean report does not prove an effect is right (it can compute
the wrong thing without failing); compare against console footage for that.
"""

import collections
import json
import re
import sys
from pathlib import Path

LOG_CHECKS = {
    "rejected shaders": r"rejected",
    "dropped draws": r"[Dd]ropp?ed .*(draw|dispatch)|GraphicsPipelineMissing",
    "unbacked images": r"does not fit; using an unbacked",
    "device lost / Vulkan errors": r"DEVICE_LOST|VK_ERROR_(?!OUT_OF_DATE)",
    "unimplemented service calls": r"Unknown / unimplemented function",
    "invalid network output": r"InvalidOutput|OutputMismatch|output had invalid values",
}


def manifest_checks(capture: Path):
    entries = [json.loads(line) for line in open(capture / "manifest.jsonl", encoding="utf-8")]
    nan, flat, zero_handles, skipped = [], [], [], collections.Counter()
    for e in entries:
        kind = e.get("kind")
        where = f"{kind} {e.get('pass', e.get('dispatch'))} {e.get('format')} {e.get('width')}x{e.get('height')}"
        if kind in ("pass", "dispatch"):
            if any(e.get("nan") or []) or any(e.get("inf") or []):
                nan.append(where)
            lo, hi = e.get("min"), e.get("max")
            # A flat output that is not a cleared black/white target is suspicious.
            if lo and hi and lo == hi and any(v not in (0, 1, None) for v in hi):
                flat.append(where + f" = {hi}")
        if e.get("zero_handles"):
            zero_handles.append(f"pass {e.get('pass')} draw {e.get('draw')}")
        if e.get("skipped") and e.get("skipped") != "recorded":
            skipped[e["skipped"]] += 1
    summary = next((e for e in entries if e.get("kind") == "summary"), {})
    return {
        "NaN/Inf outputs": nan,
        "constant non-clear outputs": flat,
        "draws with zero texture handles": zero_handles,
    }, skipped, summary


def log_checks(log: Path):
    text = log.read_text(encoding="utf-8", errors="replace").splitlines()
    found = {}
    for name, pattern in LOG_CHECKS.items():
        regex = re.compile(pattern)
        hits = [line for line in text if regex.search(line)]
        found[name] = hits
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    capture = Path(sys.argv[1])
    problems = 0
    checks, skipped, summary = manifest_checks(capture)
    print(f"capture {capture.name}: {summary.get('passes')} passes, {summary.get('draws')} draws, "
          f"{summary.get('dispatches')} dispatches{' (truncated)' if summary.get('truncated') else ''}")
    if len(sys.argv) > 2:
        checks.update(log_checks(Path(sys.argv[2])))
    for name, hits in checks.items():
        unique = list(dict.fromkeys(re.sub(r"^\[[^\]]*\] ", "", h)[:160] for h in hits))
        status = "ok" if not hits else f"{len(hits)} FOUND"
        if hits and name != "unimplemented service calls":
            problems += 1
        print(f"  {name:32} {status}")
        for line in unique[:5]:
            print(f"      {line}")
    if skipped:
        print(f"  not read back (dump limits, not errors): {dict(skipped)}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
