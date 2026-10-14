#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Spike S-1 cross-check: vendor analysis-software CSV exports vs tools/ld_ref_decode.py.

Finds CSV files whose first line contains the MoTeC CSV format name, reads their
preamble, matches each to a corpus .ld by channel names, duration and device, and
compares values at rows whose time falls exactly on a sample of the channel
(t * freq integral). Printed values are rounded by the exporter, so the tolerance
per cell is half a unit of the last printed digit.

  python -I tools/spikes/s1/compare_vendor_csv.py [--roots DIR ...] [--max-csv-mb N]

Writes fixtures/private/s1/compare_vendor_csv.json (gitignored).
"""
import argparse
import csv
import io
import json
import math
import os
import sys

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
DEFAULT_ROOTS = [os.path.expanduser("~/" + d) for d in ("Documents", "Downloads", "Desktop")] + ["C:/MoTeC"]
FORMAT_TAG = b"MoTeC CSV File"


def find_csvs(roots, max_mb):
    for r in roots:
        for dp, dn, fn in os.walk(r):
            dn[:] = [d for d in dn if d not in ("node_modules", ".git", "AppData")]
            for f in fn:
                if not f.lower().endswith(".csv"):
                    continue
                p = os.path.join(dp, f)
                try:
                    if os.path.getsize(p) > max_mb * 1e6:
                        continue
                    with open(p, "rb") as fh:
                        if FORMAT_TAG in fh.read(64):
                            yield os.path.abspath(p)
                except OSError:
                    continue


def read_preamble(p):
    """Return (meta dict, names, units, data_start_line) reading at most ~40 lines."""
    meta = {}
    with open(p, "r", encoding="cp1252", errors="replace", newline="") as fh:
        lines = []
        for _ in range(40):
            ln = fh.readline()
            if not ln:
                break
            lines.append(ln)
    rows = list(csv.reader(io.StringIO("".join(lines))))
    i = 0
    while i < len(rows) and rows[i]:
        r = rows[i]
        for c in (0, 4):
            if len(r) > c + 1 and r[c]:
                meta[r[c]] = r[c + 1:c + 3]
        i += 1
    while i < len(rows) and not rows[i]:
        i += 1
    names = rows[i] if i < len(rows) else []
    units = rows[i + 1] if i + 1 < len(rows) else []
    j = i + 2
    while j < len(rows) and not rows[j]:
        j += 1
    return meta, names, units, j


def decimals(s):
    s = s.strip()
    if "e" in s.lower():
        return None
    k = s.find(".")
    return 0 if k < 0 else len(s) - k - 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="*", default=DEFAULT_ROOTS)
    ap.add_argument("--max-csv-mb", type=float, default=200)
    a = ap.parse_args()
    summary = json.load(open(os.path.join(OUT, "summary.json"), encoding="utf-8"))
    lds = []
    seen = set()
    for s in summary:
        r = json.load(open(os.path.join(OUT, "ref", s["slug"] + ".json"), encoding="utf-8"))
        key = r.get("sha256")
        if key in seen:
            continue
        seen.add(key)
        lds.append(r)
    results = []
    csvs = sorted(set(find_csvs(a.roots, a.max_csv_mb)))
    print("MoTeC-format CSV files found: %d" % len(csvs))
    import hashlib
    seen_csv = {}
    for p in csvs:
        h = hashlib.sha256()
        with open(p, "rb") as fh:
            for blk in iter(lambda: fh.read(1 << 24), b""):
                h.update(blk)
        digest = h.hexdigest()
        if digest in seen_csv:
            results.append({"csv": p, "same_content_as": seen_csv[digest]})
            continue
        seen_csv[digest] = p
        meta, names, units, start = read_preamble(p)
        dur = None
        try:
            dur = float(meta.get("Duration", [""])[0])
        except ValueError:
            pass
        device = (meta.get("Device") or [""])[0]
        nameset = set(n for n in names if n and n != "Time")
        cands = []
        for r in lds:
            cn = set(c["name"] for c in r.get("channels", []))
            if not cn or not nameset:
                continue
            ov = len(nameset & cn) / max(1, len(nameset))
            dd = abs((r.get("duration_s") or 0) - dur) if dur is not None else 1e9
            if ov >= 0.8 and dd <= 1.5:
                cands.append((ov, -dd, r))
        rec = {"csv": p, "size": os.path.getsize(p), "device": device, "duration": dur,
               "rate": (meta.get("Sample Rate") or [""])[0], "log_date": (meta.get("Log Date") or [""])[0],
               "log_time": (meta.get("Log Time") or [""])[0], "range": (meta.get("Range") or [""])[0],
               "channels": len(nameset)}
        if not cands:
            rec["match"] = None
            results.append(rec)
            print("  no .ld match: %s" % p)
            continue
        cands.sort(key=lambda c: (c[0], c[1]), reverse=True)
        tried = []
        for ov, ndd, r in cands[:8]:
            cmp = compare(p, start, names, r)
            cmp.update({"ld": r["file"], "overlap": round(ov, 3), "duration_diff": -ndd,
                        "ld_device": r["header"].get("device_type")})
            tried.append(cmp)
        tried.sort(key=lambda c: (c["outside"], -c["cells"]))
        best = tried[0]
        rec["match"] = best["ld"]
        rec["match_overlap"] = best["overlap"]
        rec["match_duration_diff"] = best["duration_diff"]
        rec["match_class_device"] = best["ld_device"]
        rec.update({k: v for k, v in best.items() if k not in ("ld", "overlap", "duration_diff", "ld_device")})
        rec["candidates"] = [{"ld": t["ld"], "cells": t["cells"], "outside": t["outside"]} for t in tried]
        r = {"file": best["ld"]}
        results.append(rec)
        print("  %s\n    -> %s  compared %d cells in %d channels: within tol %d, outside %d (beforeMul) | shift/mul ch %s" % (
            p, r["file"], rec["cells"], rec["channels_compared"], rec["within"], rec["outside"],
            rec.get("shift_mul_verdicts")))
    with open(os.path.join(OUT, "compare_vendor_csv.json"), "w", encoding="utf-8") as fh:
        json.dump(_strict_json(results), fh, indent=1, ensure_ascii=False, allow_nan=False)


def compare(p, start, names, r):
    chans = {c["name"]: c for c in r["channels"]}
    cols = {}
    for k, n in enumerate(names):
        if n in chans and chans[n]["freq"] > 0:
            cols.setdefault(n, k)
    tcol = names.index("Time") if "Time" in names else 0
    rows_by_ch = {n: [] for n in cols}
    with open(p, "r", encoding="cp1252", errors="replace", newline="") as fh:
        rd = csv.reader(fh)
        for _ in range(start):
            next(rd, None)
        for row in rd:
            if not row or len(row) <= tcol:
                continue
            try:
                t = float(row[tcol])
            except ValueError:
                continue
            for n, k in cols.items():
                if k >= len(row) or row[k] == "":
                    continue
                f = chans[n]["freq"]
                x = t * f
                i = round(x)
                if abs(x - i) < 1e-6 * max(1.0, abs(x)) and 0 <= i < chans[n]["count"]:
                    rows_by_ch[n].append((i, row[k]))
    idx = [chans[n]["index"] for n in cols]
    out = {"cells": 0, "within": 0, "outside": 0, "channels_compared": 0, "per_channel": {}}
    vals = {"beforeMul": ref.read_channels(r["file"], idx, order="beforeMul")}
    sm = [chans[n]["index"] for n in cols if chans[n]["shift"] != 0 and chans[n]["mul"] not in (0, 1)]
    if sm:
        vals["afterMul"] = ref.read_channels(r["file"], sm, order="afterMul")
    verdicts = {}
    for n, cells in rows_by_ch.items():
        c = chans[n]
        if not cells:
            continue
        out["channels_compared"] += 1
        pc = {}
        for order, vv in vals.items():
            if c["index"] not in vv:
                continue
            arr = vv[c["index"]][1]
            ok = bad = 0
            worst = None
            for i, s in cells:
                try:
                    pv = float(s)
                except ValueError:
                    continue
                d = decimals(s)
                tol = 0.5 * 10 ** (-d) if d is not None else 1e-6 * max(1, abs(pv))
                tol = tol * (1 + 1e-9) + 1e-12
                v = arr[i]
                err = abs(v - pv)
                if err <= tol:
                    ok += 1
                else:
                    bad += 1
                    if worst is None or err > worst["err"]:
                        worst = {"index": i, "csv": s, "ref": v, "err": err, "tol": tol}
            pc[order] = {"within": ok, "outside": bad, "worst": worst}
        out["per_channel"][n] = pc
        b = pc.get("beforeMul", {})
        out["cells"] += b.get("within", 0) + b.get("outside", 0)
        out["within"] += b.get("within", 0)
        out["outside"] += b.get("outside", 0)
        if "afterMul" in pc:
            verdicts[n] = {o: (pc[o]["within"], pc[o]["outside"]) for o in pc}
    out["shift_mul_verdicts"] = verdicts
    return out


if __name__ == "__main__":
    main()
