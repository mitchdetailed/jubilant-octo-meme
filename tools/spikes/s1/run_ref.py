#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Spike S-1: run tools/ld_ref_decode.py over the corpus and classify producers.

Reads fixtures/private/s1/corpus.json, writes per-file JSON to
fixtures/private/s1/ref/<nn>_<stem>.json and fixtures/private/s1/summary.json
(all gitignored). Classification uses header evidence only.

  python -I tools/spikes/s1/run_ref.py [--no-stats] [--max-stat-samples N] [--only SUBSTR]
"""
import argparse
import json
import math
import os
import re
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

# Writer fingerprints from public writers' documented constants (header evidence only). No fingerprint uses
# the u32 at 0x5DE (D-143): only its zero/non-zero state is ever looked at. The afonso360 writer's u32
# 0x00010064 at 0x05A is also written by the vendor PC software and the converter, so it counts only with the
# writer's 0 at 0x040 outside the logger layout (as the Esses reader decides, docs/formats/ld.md).
GOTZL = {"device_serial": 0x1F44, "device_type": "ADL", "device_version": 420, "static_0x054": 0xADB0}
RUST_CRATE_WRITER = {"unknown_0x05A": 0x00010064, "static_0x040": 0}


def classify(r):
    """Producer class from header evidence (plus the path for vendor samples) -> (class, evidence)."""
    h = r.get("header") or {}
    if not h:
        return "undecodable", ["header missing"]
    ev = []
    if all(h.get(k) == v for k, v in GOTZL.items()):
        return "sim/converter (gotzl-style default header)", ["gotzl/ldparser writer default header constants"]
    if all(h.get(k) == v for k, v in RUST_CRATE_WRITER.items()) and h.get("event_ptr", 0) <= 0x10000:
        return "converter (Rust crate writer defaults)", ["afonso360 Rust crate writer default header constants"]
    dt = h.get("device_type", "")
    ser = h.get("device_serial", 0)
    evp = h.get("event_ptr", 0)
    ev.append("device_type=%r serial=%d version=%d" % (dt, ser, h.get("device_version", 0)))
    ev.append("u16@0x40=0x%04X u16@0x54=0x%04X u32@0x5DE %s" % (
        h.get("static_0x040", 0), h.get("static_0x054", 0), "non-zero" if h.get("pro_logging") else "zero"))
    if not r.get("channels") and not r.get("layout"):
        ev.append("meta_ptr=%d outside the channel area (file size %d)" % (h.get("meta_ptr", 0), r.get("size", 0)))
        return "no channels (header-only or truncated)", ev
    config_region = evp > 0x10000
    pc_layout = evp == 0x6E2 and h.get("static_0x040") == 2 and h.get("static_0x054") == 0x0080
    ev.append("event block at 0x%X%s" % (evp, " (after a device-configuration region)" if config_region else ""))
    path = r["file"].replace("\\", "/").lower()
    if config_region:
        if h.get("static_0x054") == 0 and dt != "MTCA2LD":
            return "device-raw: logger file as downloaded", ev
        return "device-modified: logger file with rewritten header fields", ev
    if pc_layout:
        if dt == "MTCA2LD":
            if "/t2/test model/" in path:
                return "pc-layout, MTCA2LD identity: vendor T2 test model sample", ev
            return "pc-layout, MTCA2LD identity: vendor analysis-software export or user's converter", ev
        return "pc-layout, device identity kept: logger data re-written by PC software", ev
    return "unclassified (see evidence)", ev


def slug(i, path):
    stem = os.path.splitext(os.path.basename(path))[0]
    return "%02d_%s" % (i, re.sub(r"[^A-Za-z0-9._-]+", "_", stem)[:60])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-stats", action="store_true")
    ap.add_argument("--max-stat-samples", type=int, default=4_000_000)
    ap.add_argument("--only")
    a = ap.parse_args()
    with open(os.path.join(OUT, "corpus.json"), encoding="utf-8") as fh:
        corpus = json.load(fh)
    lds = [f for f in corpus if f["ext"] == ".ld"]
    os.makedirs(os.path.join(OUT, "ref"), exist_ok=True)
    summary = []
    for i, f in enumerate(lds):
        if a.only and a.only.lower() not in f["path"].lower():
            continue
        t = time.perf_counter()
        try:
            r = ref.decode_file(f["path"], max_samples=a.max_stat_samples, want_hash=False,
                                stats=not a.no_stats)
            err = None
        except Exception as ex:  # report, keep going
            r = {"file": f["path"], "size": f["size"], "error": repr(ex)}
            err = repr(ex)
        wall = (time.perf_counter() - t) * 1000
        r["sha256"] = f.get("sha256")
        r["corpus_origin"] = f["origin"]
        cls, evd = classify(r) if not err else ("error", [err])
        r["producer_class"] = cls
        r["producer_evidence"] = evd
        s = slug(i, f["path"])
        with open(os.path.join(OUT, "ref", s + ".json"), "w", encoding="utf-8") as fh:
            json.dump(_strict_json(r), fh, indent=1, ensure_ascii=False, allow_nan=False)
        h = r.get("header") or {}
        chans = r.get("channels", [])
        summary.append({
            "slug": s, "file": f["path"], "size": f["size"], "origin": f["origin"],
            "identical_to": f.get("identical_to"), "class": cls, "evidence": evd,
            "device_type": h.get("device_type"), "device_serial": h.get("device_serial"),
            "device_version": h.get("device_version"), "num_channels": h.get("num_channels"),
            "walked": len(chans), "stride": (r.get("layout") or {}).get("stride"),
            "stride_counts": (r.get("layout") or {}).get("stride_counts"),
            "counter_pattern": (r.get("layout") or {}).get("counter_pattern"),
            "dtype_pairs": sorted({"%X/%d" % (c["dtype_a"], c["dtype"]) for c in chans}),
            "freqs": sorted({c["freq"] for c in chans}),
            "shift_mul": [(c["index"], c["name"], c["shift"], c["mul"], c["scale"], c["dec"])
                          for c in chans if c["shift"] != 0 and c["mul"] not in (0, 1)],
            "shift_nonzero": sum(1 for c in chans if c["shift"] != 0),
            "mul_ne1": sum(1 for c in chans if c["mul"] not in (0, 1)),
            "duration_s": r.get("duration_s"), "timing_ms": r.get("timing_ms"), "wall_ms": round(wall, 1),
            "warning_codes": r.get("warning_codes"), "error": err,
            "ldx": bool(r.get("ldx")), "ldx_markers": len((r.get("ldx") or {}).get("markers", [])),
        })
        print("%-4s %-55s %-38s %4d ch  %-8s probe %6.1f ms  wall %7.0f ms" % (
            i, os.path.basename(f["path"])[:55], cls[:38], len(chans), h.get("device_type", ""),
            (r.get("timing_ms") or {}).get("probe", -1), wall), flush=True)
    name = "summary.json" if not a.only else "summary_partial.json"
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as fh:
        json.dump(_strict_json(summary), fh, indent=1, ensure_ascii=False, allow_nan=False)


if __name__ == "__main__":
    main()
