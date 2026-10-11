#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Spike S-1: probe timing (header + blocks + channel walk, no sample decode) per file.

Median of 5 warm runs of ld_ref_decode.decode_file(stats=False) per distinct .ld,
plus one full decode (exact statistics) for comparison. Python timings are
informative only; the AC-M0-12 budget applies to the C++ reader.

  python -I tools/spikes/s1/probe_timing.py
"""
import json
import math
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import ld_ref_decode as ref  # noqa: E402


def _strict_json(v):
    """NaN and infinities become null: the spike outputs are strict JSON (RFC 8259), as every JSON file under
    fixtures/ must be for `esses-cli validate` (AC-M0-10)."""
    if isinstance(v, float) and not math.isfinite(v):
        return None
    if isinstance(v, dict):
        return {k: _strict_json(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_strict_json(x) for x in v]
    return v

OUT = os.path.join(REPO, "fixtures", "private", "s1")


def main():
    corpus = json.load(open(os.path.join(OUT, "corpus.json"), encoding="utf-8"))
    rows = []
    seen = set()
    for f in corpus:
        if f["ext"] != ".ld" or f.get("sha256") in seen:
            continue
        seen.add(f.get("sha256"))
        probe = []
        for _ in range(5):
            r = ref.decode_file(f["path"], want_hash=False, stats=False, ldx=False)
            probe.append(r["timing_ms"]["probe"])
        t = time.perf_counter()
        full = ref.decode_file(f["path"], want_hash=False, stats=True, ldx=True, max_samples=0)
        wall = (time.perf_counter() - t) * 1000
        rows.append({"file": f["path"], "size": f["size"], "channels": len(full.get("channels", [])),
                     "probe_ms_median": round(statistics.median(probe), 2),
                     "full_exact_ms": round(wall, 1)})
        print("%8.2f ms probe  %9.1f ms full  %10d B  %s" % (rows[-1]["probe_ms_median"], wall, f["size"],
                                                              os.path.basename(f["path"])), flush=True)
    with open(os.path.join(OUT, "probe_timing.json"), "w", encoding="utf-8") as fh:
        json.dump(_strict_json(rows), fh, indent=1, allow_nan=False)


if __name__ == "__main__":
    main()
