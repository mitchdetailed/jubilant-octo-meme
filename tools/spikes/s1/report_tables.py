#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Spike S-1: print the markdown tables used in docs/spikes/S-1.md from the private results.

  python -I tools/spikes/s1/report_tables.py > <scratch file>
"""
import collections
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(REPO, "fixtures", "private", "s1")
HOME = os.path.expanduser("~") + "\\"

SHORT = {
    "device-raw: logger file as downloaded": "logger, as downloaded",
    "device-modified: logger file with rewritten header fields": "logger, header-modified copy",
    "pc-layout, device identity kept: logger data re-written by PC software": "PC layout, logger identity",
    "pc-layout, MTCA2LD identity: vendor analysis-software export or user's converter": "PC layout, generic identity",
    "pc-layout, MTCA2LD identity: vendor T2 test model sample": "PC layout, generic identity (vendor T2 sample)",
    "no channels (header-only or truncated)": "no channels",
}


def short_path(p):
    p = p.replace(os.environ.get("ESSES_CONVERTER_REF_DIR", HOME + "converter\\Reference information\\"), "conv-ref\\")
    p = p.replace(HOME, "~\\")
    return p


def main():
    summary = json.load(open(os.path.join(OUT, "summary.json"), encoding="utf-8"))
    timing = {t["file"]: t for t in json.load(open(os.path.join(OUT, "probe_timing.json"), encoding="utf-8"))}
    print("| # | File | MB | Class | Device | Ch | Record gaps | dtype pairs | .ldx | Probe ms | Full ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for i, s in enumerate(summary):
        t = timing.get(s["file"])
        dup = " (same bytes as #%s)" % ", ".join(
            str(j) for j, o in enumerate(summary) if s["identical_to"] and o["file"] in s["identical_to"]) \
            if s["identical_to"] else ""
        sc = s["stride_counts"] or {}
        gaps = "124" if list(sc) in ([], ["124"]) else ("124/212" if set(sc) <= {"124", "212"} else "variable")
        print("| %d | `%s`%s | %.1f | %s | %s | %d | %s | %s | %s | %s | %s |" % (
            i, short_path(s["file"]), dup, s["size"] / 1e6, SHORT.get(s["class"], s["class"]), s["device_type"],
            s["walked"], gaps, " ".join(s["dtype_pairs"]), "yes" if s["ldx"] else "-",
            ("%.2f" % t["probe_ms_median"]) if t else "dup", ("%.0f" % t["full_exact_ms"]) if t else "dup"))
    print()
    c = collections.Counter(SHORT.get(s["class"], s["class"]) for s in summary)
    for k, v in c.most_common():
        print("- %s: %d" % (k, v))


if __name__ == "__main__":
    main()
