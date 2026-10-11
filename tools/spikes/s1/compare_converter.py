#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Spike S-1 cross-check: user's converter .ld parser vs tools/ld_ref_decode.py.

Compares OUTPUTS only (channel lists and decoded values); no converter code is copied
into Esses (D-041). The converter is imported from its own directory; it needs numpy,
which lives in the user site-packages that ``python -I`` hides, so that directory is
appended to sys.path explicitly (appended, so nothing there shadows the stdlib).

  python -I tools/spikes/s1/compare_converter.py [--only SUBSTR] [--full-limit N]

Reads fixtures/private/s1/corpus.json and ref/*.json (run_ref.py first); writes
fixtures/private/s1/compare_converter.json (gitignored).
"""
import argparse
import contextlib
import io
import json
import math
import os
import site
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import ld_ref_decode as ref  # noqa: E402

CONV_DIR = ("C:/Users/mitch/Documents/AntiGravity Projects/QT C++ Log Viewer/"
            "Reference projects/V8 of log file converter")
sys.dont_write_bytecode = True        # never write .pyc files into the converter's folder
sys.path.append(site.getusersitepackages())
sys.path.append(CONV_DIR)
import numpy as np  # noqa: E402
from parsers.ld_parser import LdParser  # noqa: E402


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


def ulp_diff32(a, b):
    ai = a.view(np.int32).astype(np.int64)
    bi = b.view(np.int32).astype(np.int64)
    ai = np.where(ai < 0, -(ai & 0x7FFFFFFF), ai)
    bi = np.where(bi < 0, -(bi & 0x7FFFFFFF), bi)
    return np.abs(ai - bi)


def compare_values(refvals, conv):
    r = np.frombuffer(refvals, dtype=np.float64)
    c = np.asarray(conv)
    res = {"ref_n": int(r.size), "conv_n": int(c.size), "conv_dtype": str(c.dtype)}
    n = min(r.size, c.size)
    r = r[:n]
    c = c[:n]
    if c.dtype == np.float32:
        r32 = r.astype(np.float32)
        c32 = c
    else:
        r32 = r.astype(np.float32)
        c32 = c.astype(np.float32)
    rn = np.isnan(r32)
    cn = np.isnan(c32)
    both_nan = rn & cn
    eq = (r32 == c32) | both_nan
    res["conv_nan"] = int(cn.sum())
    res["ref_nan"] = int(rn.sum())
    res["equal"] = int(eq.sum())
    res["mismatch"] = int(n - eq.sum())
    fin = ~(rn | cn)
    if fin.any():
        u = ulp_diff32(r32[fin], c32[fin])
        res["max_ulp_f32"] = int(u.max())
        res["n_gt_1ulp"] = int((u > 1).sum())
        res["max_abs"] = float(np.max(np.abs(r[fin] - c[fin].astype(np.float64))))
    if res["mismatch"]:
        idx = np.nonzero(~eq)[0]
        k = int(idx[0])
        res["first_mismatch"] = {"index": k, "ref": float(r[k]), "conv": float(c[k])}
        res["mismatch_nan_only"] = bool(np.all(cn[~eq] | rn[~eq]))
    return res


def stats_of(arr):
    a = np.asarray(arr, dtype=np.float64)
    f = a[np.isfinite(a)]
    return {"n": int(a.size), "min": float(f.min()) if f.size else None,
            "max": float(f.max()) if f.size else None, "mean": float(f.mean()) if f.size else None,
            "first": [float(x) for x in a[:5]], "last": [float(x) for x in a[-5:]]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only")
    ap.add_argument("--full-limit", type=int, default=5_000_000,
                    help="channels with more samples are compared by statistics only")
    a = ap.parse_args()
    corpus = json.load(open(os.path.join(OUT, "corpus.json"), encoding="utf-8"))
    summary = json.load(open(os.path.join(OUT, "summary.json"), encoding="utf-8"))
    by_path = {s["file"]: s for s in summary}
    done = {}
    results = []
    for f in [x for x in corpus if x["ext"] == ".ld"]:
        if a.only and a.only.lower() not in f["path"].lower():
            continue
        s = by_path.get(f["path"])
        if s is None:
            continue
        rec = {"file": f["path"], "slug": s["slug"], "class": s["class"]}
        if f.get("sha256") in done:
            rec["same_content_as"] = done[f["sha256"]]
            results.append(rec)
            continue
        done[f.get("sha256")] = f["path"]
        refj = json.load(open(os.path.join(OUT, "ref", s["slug"] + ".json"), encoding="utf-8"))
        rch = refj.get("channels", [])
        t0 = time.perf_counter()
        sink = io.StringIO()
        try:
            with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
                p = LdParser(f["path"])
                cch = p.parse()
        except Exception as ex:
            rec["conv_error"] = repr(ex)
            results.append(rec)
            print("%-55s converter error: %r" % (os.path.basename(f["path"])[:55], ex), flush=True)
            continue
        rec["conv_parse_ms"] = round((time.perf_counter() - t0) * 1000, 1)
        rec["ref_channels"] = len(rch)
        rec["conv_channels"] = len(cch)
        # align by data pointer (unique per channel), fall back to index
        cby = {}
        for k, c in enumerate(cch):
            cby.setdefault(c.get("data_ptr"), []).append(k)
        pairs = []
        for i, e in enumerate(rch):
            ks = cby.get(e["data_ptr"], [])
            k = ks.pop(0) if ks else None
            pairs.append((i, k))
        matched = {k for _, k in pairs if k is not None}
        rec["ref_only"] = [rch[i]["name"] for i, k in pairs if k is None]
        rec["conv_only"] = [cch[k].get("name") for k in range(len(cch)) if k not in matched]
        fields = []
        valres = []
        need_full = [i for i, k in pairs if k is not None and rch[i]["count"] <= a.full_limit]
        orders_needed = {}
        for i in need_full:
            e = rch[i]
            orders_needed[i] = ["beforeMul", "afterMul"] if (e["shift"] != 0 and e["mul"] not in (0, 1)) else ["beforeMul"]
        refvals = {}
        for o in ("beforeMul", "afterMul"):
            idx = [i for i in need_full if o in orders_needed[i]]
            if idx:
                for i, (_, vals) in ref.read_channels(f["path"], idx, order=o).items():
                    refvals[(i, o)] = vals
        for i, k in pairs:
            if k is None:
                continue
            e = rch[i]
            c = cch[k]
            diffs = {}
            cmpmap = [("name", "name"), ("count", "sample_count"), ("freq", "freq"), ("dtype_a", "type_id"),
                      ("dtype", "size"), ("mul", "mul"), ("scale", "div"), ("shift", "add"), ("dec", "exp"),
                      ("counter", "id"), ("unit", "unit"), ("short_name", "short_name")]
            for rk, ck in cmpmap:
                if c.get(ck) != e.get(rk):
                    diffs[rk] = [e.get(rk), c.get(ck)]
            if diffs:
                fields.append({"index": i, "name": e["name"], "diffs": diffs})
            with contextlib.redirect_stdout(sink), contextlib.redirect_stderr(sink):
                try:
                    arr = p.extract_data(c)
                except Exception as ex:
                    valres.append({"index": i, "name": e["name"], "conv_extract_error": repr(ex)})
                    continue
            if arr is None:
                valres.append({"index": i, "name": e["name"], "conv_extract": None})
                continue
            vr = {"index": i, "name": e["name"]}
            if (i, "beforeMul") in refvals:
                for o in orders_needed[i]:
                    vr[o] = compare_values(refvals[(i, o)], arr)
            else:
                vr["stats_only"] = True
                vr["conv_stats"] = stats_of(arr)
                vr["ref_stats"] = e.get("stats", {}).get("beforeMul")
            valres.append(vr)
        rec["field_diffs"] = fields
        rec["values"] = valres
        bad = [v for v in valres if any(isinstance(v.get(o), dict) and v[o].get("mismatch") for o in ("beforeMul",))]
        rec["channels_with_value_mismatch"] = len(bad)
        rec["channels_value_equal"] = sum(1 for v in valres if isinstance(v.get("beforeMul"), dict) and not v["beforeMul"].get("mismatch"))
        rec["wall_ms"] = round((time.perf_counter() - t0) * 1000, 1)
        results.append(rec)
        print("%-55s ref %4d conv %4d  field-diff ch %4d  value-equal %4d  value-mismatch %4d  %6.0f ms" % (
            os.path.basename(f["path"])[:55], len(rch), len(cch), len(fields), rec["channels_value_equal"],
            len(bad), rec["wall_ms"]), flush=True)
    name = "compare_converter.json" if not a.only else "compare_converter_partial.json"
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as fh:
        json.dump(_strict_json(results), fh, indent=1, ensure_ascii=False, default=str, allow_nan=False)


if __name__ == "__main__":
    main()
