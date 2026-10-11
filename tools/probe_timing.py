#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""AC-M0-12 timing: `esses-cli info` on the ~500 MB synthetic .ld, median of N runs (D-042: absolute limit on
the reference machine).

Writes the file with `esses-gen-fixtures --large` when it does not exist, runs one warm-up and then --runs times
`esses-cli info <file> --json --timing`, and reports the median wall time of the whole command (process start,
unit table, sniff, probe, probe_json) and the median of the sniff + probe time the CLI measures itself. Exit 1
when the median wall time exceeds --limit-ms. The OS file cache is warm after the first run (a cold-cache figure
needs the cache flushed by hand).

Standard library only:
  python -I tools/probe_timing.py --cli <esses-cli> --gen <esses-gen-fixtures> --file <path.ld> [--size-mb 500]
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import statistics
import subprocess
import sys
import time


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cli", required=True)
    ap.add_argument("--gen", help="esses-gen-fixtures (to write --file when absent)")
    ap.add_argument("--file", required=True)
    ap.add_argument("--size-mb", type=int, default=500)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--limit-ms", type=float, default=200.0)
    a = ap.parse_args(argv)
    a.cli = str(pathlib.Path(a.cli).resolve())  # Windows CreateProcess does not search relative forward-slash paths
    a.gen = str(pathlib.Path(a.gen).resolve()) if a.gen else None
    path = pathlib.Path(a.file)
    if not path.exists():
        if not a.gen:
            print(f"probe_timing: {path} does not exist and --gen is not given")
            return 2
        path.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([a.gen, "--large", str(path), "--large-mb", str(a.size_mb)])
        if r.returncode != 0:
            return 2
    size = os.path.getsize(path)
    walls, probes = [], []
    for k in range(a.runs + 1):
        t0 = time.perf_counter()
        r = subprocess.run([a.cli, "info", str(path), "--json", "--timing"], capture_output=True)
        wall = (time.perf_counter() - t0) * 1000.0
        if r.returncode != 0:
            print(r.stderr.decode("utf-8", "replace"))
            return 2
        m = re.search(r"probe: ([0-9.]+) ms", r.stderr.decode("utf-8", "replace"))
        doc = json.loads(r.stdout.decode("utf-8"))
        channels = sum(len(layer.get("channels", [])) for layer in doc.get("layers", []))
        if k == 0:
            continue  # warm-up
        walls.append(wall)
        probes.append(float(m.group(1)) if m else float("nan"))
    result = {"file": str(path), "bytes": size, "channels": channels, "runs": a.runs,
              "wallMs": [round(w, 2) for w in walls], "probeMs": [round(p, 2) for p in probes],
              "medianWallMs": round(statistics.median(walls), 2), "medianProbeMs": round(statistics.median(probes), 2),
              "limitMs": a.limit_ms}
    print(json.dumps(result, indent=1))
    return 0 if result["medianWallMs"] <= a.limit_ms else 1


if __name__ == "__main__":
    sys.exit(main())
