#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 the Esses contributors
# SPDX-License-Identifier: MIT
"""Private fixture manifest for the user's real .ld files (AC-M0-02 private run; 08 section 4.5, D-005).

Reads fixtures/private/s1/corpus.json (spike S-1's inventory of the user's files, never committed) and writes,
under fixtures/private/motec.ld/ (git-ignored):

  <id>/fixture.json            fixture-1, private, provenance from the S-1 producer class, oracle ref-decode with
                               oracle.validity finite-only (the reference decoder does not decode gap blocks)
  <id>/expected.channels/      the oracle's values for windows of every channel: the first W samples, W samples
                               spread evenly across the channel and the last W (always the last sample, so the
                               reader's count is checked), computed by tools/ld_ref_decode.py's own functions
                               (walk_channels, classify_dtype, make_scaler, time_us) exactly as its --dump-channel
                               CSVs are, scaling order afterMul (D-037)
  manifest.json                {"fixtures": [{"dir": "<id>", "input": "<the real file>"}]}

The inputs stay where they are (the manifest points at them). Then, unless --no-update, it runs
`esses-cli fixture verify --manifest manifest.json --update`, which writes expected.probe.json,
expected.markers.json, expected.laps.json and the dialect from the reader (regression goldens; the channel values
never come from the reader, D-041). `ctest -R fixtures.verify_private` re-runs the verification.

Standard library only; run with python -I. The oracle is imported read-only and never modified (D-041, D-144).
  python -I tools/build_private_fixtures.py --root . --cli build/<dir>/bin/esses-cli [--window 64]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import mmap
import os
import pathlib
import re
import shutil
import subprocess
import sys

KEEP = set(b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ._-()+,=@[]{}#&!'~")


def percent_encode(name: str) -> str:
    """The file-name encoding of esses-cli (apps/cli/src/cli_common.cpp percentEncode)."""
    raw = name.encode("utf-8")
    out = []
    for i, b in enumerate(raw):
        last = i + 1 == len(raw)
        if b in KEEP and not (last and b in (ord(" "), ord("."))):
            out.append(chr(b))
        else:
            out.append("%%%02X" % b)
    return "".join(out)


def sha256_of(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def companion_of(path: pathlib.Path) -> pathlib.Path | None:
    stem = path.stem.lower()
    for p in sorted(path.parent.iterdir()):
        if p != path and p.suffix.lower() == ".ldx" and p.stem.lower() == stem:
            return p
    return None


def window_indices(n: int, w: int) -> list[int]:
    if n <= 0:
        return []
    idx = set(range(min(n, w))) | set(range(max(0, n - w), n))
    if n > 1:
        for k in range(w):
            idx.add((k * (n - 1)) // max(1, w - 1))
    return sorted(idx)


def oracle_windows(oracle, path: pathlib.Path, window: int, order: str) -> list[tuple[int, str, list[str]]]:
    """[(oracle index, oracle name, csv rows)] using the oracle's functions as its dumps do."""
    out = []
    with open(path, "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            size = len(mm)
            if size < oracle.HEADER_SIZE:
                return out
            meta = oracle.u32(mm, 0x08)
            if not (oracle.HEADER_SIZE <= meta < size):
                return out  # the oracle decodes no channel (ld.meta_ptr_out_of_range)
            entries, _ = oracle.walk_channels(mm, size, meta, oracle.Log(), "auto")
            if entries:
                t = entries[-1]
                if (t["next_ptr"] == 0 and t["count"] == 0 and t["freq"] == 0 and t["dtype"] == 0
                        and t["dtype_a"] == 0 and not t["name"]):
                    entries.pop()  # the terminator record, as the oracle drops it
            for i, e in enumerate(entries):
                kind, code, _ = oracle.classify_dtype(e["dtype_a"], e["dtype"])
                if kind is None and e["dtype"] in (1, 2, 4, 8):
                    code = {1: "b", 2: "h", 4: "i", 8: "q"}[e["dtype"]]
                if not code:
                    continue  # the oracle dumps no channel it cannot decode
                w = oracle.raw_width(code)
                fit = max(0, (size - e["data_ptr"]) // w) if e["data_ptr"] < size else 0
                n = min(e["count"], fit)
                f = oracle.make_scaler(e, order)
                rows = []
                with memoryview(mm) as base:
                    view = base[e["data_ptr"]:e["data_ptr"] + n * w].cast(code)
                    try:
                        for k in window_indices(n, window):
                            v = f(view[k])
                            rows.append("%d,%d,%r,%d\n" % (k, oracle.time_us(k, e["freq"]), v,
                                                            1 if math.isfinite(v) else 0))
                    finally:
                        view.release()
                out.append((i, e["name"], rows))
        finally:
            mm.close()
    return out


def provenance_of(path: pathlib.Path, ref: dict | None) -> str:
    text = str(path).replace("\\", "/").lower()
    if "/t2/" in text or "/samples/" in text:
        return "vendor-sample"  # D-146
    cls = (ref or {}).get("producer_class", "") or ""
    if cls.lower().startswith("logger") or "logger identity" in cls.lower():
        return "user-device-log"
    return "user-tool-output"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=".", help="repository root")
    ap.add_argument("--corpus", help="corpus.json (default fixtures/private/s1/corpus.json)")
    ap.add_argument("--out", help="output directory (default fixtures/private/motec.ld)")
    ap.add_argument("--window", type=int, default=64, help="samples per window (default 64)")
    ap.add_argument("--cli", help="esses-cli for the --update run")
    ap.add_argument("--no-update", action="store_true", help="do not run esses-cli fixture verify --update")
    a = ap.parse_args(argv)
    root = pathlib.Path(a.root).resolve()
    corpus_path = pathlib.Path(a.corpus) if a.corpus else root / "fixtures/private/s1/corpus.json"
    out = pathlib.Path(a.out) if a.out else root / "fixtures/private/motec.ld"
    if not corpus_path.exists():
        print(f"build_private_fixtures: {corpus_path} absent; nothing to do (private corpus not checked out)")
        return 0
    sys.path.insert(0, str(root / "tools"))
    import ld_ref_decode as oracle  # noqa: E402 (the independent oracle, imported read-only)

    refs = {}
    ref_dir = corpus_path.parent / "ref"
    if ref_dir.is_dir():
        for p in ref_dir.glob("*.json"):
            m = re.match(r"^(\d+)_", p.name)
            if m:
                refs[int(m.group(1))] = p
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    lds = [e for e in corpus if e.get("ext") == ".ld"]
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"fixtures": []}
    missing = 0
    channels = 0
    for k, entry in enumerate(lds):
        src = pathlib.Path(entry["path"])
        if not src.exists():
            print(f"  missing: {src}")
            missing += 1
            continue
        fid = "s1-%02d-%s" % (k, re.sub(r"[^a-z0-9._-]+", "-", src.stem.lower()).strip("-.") or "file")
        d = out / fid
        if d.exists():
            shutil.rmtree(d)
        (d / "expected.channels").mkdir(parents=True)
        ref = None
        if k in refs:
            try:
                ref = json.loads(refs[k].read_text(encoding="utf-8"))
            except (OSError, ValueError):
                ref = None
        fixture = {
            "$schema": "urn:esses:fixture-1", "schemaVersion": 1, "format": "motec.ld", "name": fid,
            "input": {"file": "input.ld", "sha256": entry.get("sha256") or sha256_of(src), "bytes": src.stat().st_size},
            "source": "the user's file %s from the spike S-1 inventory (fixtures/private/s1/corpus.json); never "
                      "committed" % src.name,
            "provenance": provenance_of(src, ref),
            "licence": "LicenseRef-unknown",
            "permission": "private: the user's own files, nothing real is public for now (Q-04; D-005, D-145, D-146)",
            "private": True,
            "importOptions": {"ld": {"shiftOrder": "afterMul"}},
            "oracle": {"kind": "ref-decode",
                       "ref": "tools/ld_ref_decode.py %s (MIT), afterMul, windows of %d samples (first, spread, last)"
                              % (oracle.TOOL_VERSION, a.window),
                       "validity": "finite-only",
                       "tolerance": {"F32": "1ulp", "F64": "1ulp", "I32": "exact", "timesUs": "exact"}},
            "channelsCompared": "all",
        }
        comp = companion_of(src)
        if comp is not None:
            fixture["companions"] = [{"file": "input.ldx", "sha256": sha256_of(comp), "bytes": comp.stat().st_size}]
        (d / "fixture.json").write_text(json.dumps(fixture, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                                        newline="\n")
        for index, name, rows in oracle_windows(oracle, src, a.window, "afterMul"):
            fname = "%03d_%s.csv" % (index, percent_encode(name))
            with open(d / "expected.channels" / fname, "w", encoding="ascii", newline="\n") as fh:
                fh.write("index,timeUs,value,valid\n")
                fh.writelines(rows)
            channels += 1
        manifest["fixtures"].append({"dir": fid, "input": str(src)})
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8",
                                       newline="\n")
    print(f"build_private_fixtures: {len(manifest['fixtures'])} fixtures, {channels} channel windows, "
          f"{missing} missing inputs -> {out}")
    if a.no_update:
        return 0
    if not a.cli:
        print("build_private_fixtures: pass --cli <esses-cli> to write the probe/markers/laps goldens")
        return 0
    r = subprocess.run([str(pathlib.Path(a.cli).resolve()), "fixture", "verify", "--manifest", str(out / "manifest.json"), "--update"])
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
